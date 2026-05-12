// Unit tests for SrTaskService.
//
// The service owns the lifecycle of every running SR job and emits
// completion/failure events on poll().  These tests use a FakeEngine
// that exposes setters so we can drive the engine through the four
// status transitions (Idle -> Running -> Completed / Failed) without
// spawning a real SR process.
//
// Coverage:
//   * factory returning nullptr -> ok=false, error_message non-empty
//   * engine returning false from start_inference -> ok=false, error
//     mirrors last_error().description
//   * successful start -> task added, status_msg includes filename
//   * poll() emits SrCompletion when engine reaches Completed and
//     erases the task
//   * poll() emits SrFailure when engine reaches Failed and erases
//     the task
//   * poll() leaves Running tasks in place and updates status_msg
//     according to engine progress
//   * has_running() / running_count() reflect engine status
//   * cancel_all() invokes engine->cancel() on every task
//   * destructor calls cancel_all() so engines tear down cleanly

#include <catch2/catch_test_macros.hpp>

#include "domain/sr_task_service.h"
#include "app/sr_dialog.h"
#include "app/sr_infer_engine.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>

using idiff::SrCompletion;
using idiff::SrFailure;
using idiff::SrTaskService;
using idiff::SREngineStatus;
using idiff::SRError;
using idiff::SRInferEngine;
using idiff::SRTaskParams;

namespace {

class FakeEngine : public SRInferEngine {
public:
    // Configurable behaviour for the next start_inference() call.
    bool start_returns = true;
    SRError start_error{"test", "synthetic start failure"};

    // State the test driver mutates between poll()s.
    std::atomic<SREngineStatus> status{SREngineStatus::Idle};
    std::atomic<float> progress{-1.0f};
    std::filesystem::path output_path;
    SRError final_error{"test", "synthetic engine failure"};

    // Observability for assertions.  The `*_external` pointers let
    // tests survive the engine being deleted by ~SrTaskService: the
    // counters live on the test's stack and outlive the FakeEngine.
    int start_calls = 0;
    int cancel_calls = 0;
    int* start_calls_external = nullptr;
    int* cancel_calls_external = nullptr;

    bool start_inference(const std::filesystem::path& in,
                         const std::filesystem::path& out,
                         int /*scale*/, int /*tile_size*/,
                         int /*tile_overlap*/,
                         const std::string& /*model*/,
                         const std::string& /*color_correction*/) override {
        ++start_calls;
        if (start_calls_external) ++(*start_calls_external);
        last_input = in;
        if (output_path.empty()) output_path = out;
        if (!start_returns) {
            status = SREngineStatus::Failed;
            return false;
        }
        status = SREngineStatus::Running;
        return true;
    }

    SREngineStatus get_status() const override { return status.load(); }
    std::filesystem::path get_output_path() const override {
        return output_path;
    }
    bool cancel() override {
        ++cancel_calls;
        if (cancel_calls_external) ++(*cancel_calls_external);
        status = SREngineStatus::Idle;
        return true;
    }
    SRError last_error() const override { return final_error; }
    float get_progress() const override { return progress.load(); }

    std::filesystem::path last_input;
};

// Locate a fake-engine pointer back from a service: tests own the
// engine lifecycle until SrTaskService::start consumes it, so we
// stash a raw pointer in an out-param before handing the unique_ptr
// over to the service.
SRTaskParams make_params(const std::string& name) {
    SRTaskParams p;
    p.input_path = "/tmp/" + name + ".png";
    p.output_path = "/tmp/" + name + "_sr.png";
    p.scale = 2;
    return p;
}

} // namespace

TEST_CASE("SrTaskService starts empty", "[sr_task_service]") {
    SrTaskService svc;
    REQUIRE(svc.empty());
    REQUIRE(svc.size() == 0);
    REQUIRE_FALSE(svc.has_running());
    REQUIRE(svc.running_count() == 0);
}

TEST_CASE("start: factory returning null surfaces an error", "[sr_task_service]") {
    SrTaskService svc;
    auto factory = []() -> std::unique_ptr<SRInferEngine> { return nullptr; };

    auto result = svc.start(make_params("a"), factory);

    REQUIRE_FALSE(result.ok);
    REQUIRE_FALSE(result.error_title.empty());
    REQUIRE_FALSE(result.error_message.empty());
    REQUIRE(svc.empty());
}

TEST_CASE("start: engine refusing the job surfaces last_error", "[sr_task_service]") {
    SrTaskService svc;
    auto fake = std::make_unique<FakeEngine>();
    fake->start_returns = false;
    fake->final_error = {"test", "out of memory"};
    int start_calls = 0;
    fake->start_calls_external = &start_calls;
    auto factory = [&]() -> std::unique_ptr<SRInferEngine> {
        return std::move(fake);
    };

    auto result = svc.start(make_params("a"), factory);

    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error_message.find("out of memory") != std::string::npos);
    REQUIRE(svc.empty());
    REQUIRE(start_calls == 1);
}

TEST_CASE("start: successful engine adds a task", "[sr_task_service]") {
    SrTaskService svc;
    auto fake = std::make_unique<FakeEngine>();
    auto* fake_raw = fake.get();
    auto factory = [&]() -> std::unique_ptr<SRInferEngine> {
        return std::move(fake);
    };

    auto result = svc.start(make_params("kitten"), factory);

    REQUIRE(result.ok);
    REQUIRE(svc.size() == 1);
    REQUIRE(svc.tasks().front().input_path == "/tmp/kitten.png");
    REQUIRE(svc.tasks().front().status_msg.find("kitten.png") !=
            std::string::npos);
    REQUIRE(svc.has_running());
    REQUIRE(svc.running_count() == 1);
    REQUIRE(fake_raw->last_input == std::filesystem::path("/tmp/kitten.png"));
}

TEST_CASE("poll: Running task stays and updates status_msg from progress",
          "[sr_task_service]") {
    SrTaskService svc;
    auto fake = std::make_unique<FakeEngine>();
    auto* fake_raw = fake.get();
    fake_raw->progress = 0.42f;
    auto factory = [&]() -> std::unique_ptr<SRInferEngine> {
        return std::move(fake);
    };
    REQUIRE(svc.start(make_params("a"), factory).ok);

    std::vector<SrCompletion> completions;
    std::vector<SrFailure> failures;
    svc.poll(completions, failures);

    REQUIRE(completions.empty());
    REQUIRE(failures.empty());
    REQUIRE(svc.size() == 1);
    REQUIRE(svc.tasks().front().status_msg.find("42") != std::string::npos);
}

TEST_CASE("poll: Completed task emits SrCompletion and is erased",
          "[sr_task_service]") {
    SrTaskService svc;
    auto fake = std::make_unique<FakeEngine>();
    auto* fake_raw = fake.get();
    fake_raw->output_path = "/tmp/k_sr.png";
    auto factory = [&]() -> std::unique_ptr<SRInferEngine> {
        return std::move(fake);
    };
    REQUIRE(svc.start(make_params("k"), factory).ok);
    fake_raw->status = SREngineStatus::Completed;

    std::vector<SrCompletion> completions;
    std::vector<SrFailure> failures;
    svc.poll(completions, failures);

    REQUIRE(failures.empty());
    REQUIRE(completions.size() == 1);
    REQUIRE(completions.front().input_path == "/tmp/k.png");
    REQUIRE(completions.front().output_path == "/tmp/k_sr.png");
    REQUIRE(completions.front().status_msg.find("k_sr.png") !=
            std::string::npos);
    REQUIRE(svc.empty());
}

TEST_CASE("poll: Failed task emits SrFailure and is erased",
          "[sr_task_service]") {
    SrTaskService svc;
    auto fake = std::make_unique<FakeEngine>();
    auto* fake_raw = fake.get();
    fake_raw->final_error = {"runtime", "model crashed"};
    auto factory = [&]() -> std::unique_ptr<SRInferEngine> {
        return std::move(fake);
    };
    REQUIRE(svc.start(make_params("a"), factory).ok);
    fake_raw->status = SREngineStatus::Failed;

    std::vector<SrCompletion> completions;
    std::vector<SrFailure> failures;
    svc.poll(completions, failures);

    REQUIRE(completions.empty());
    REQUIRE(failures.size() == 1);
    REQUIRE(failures.front().input_path == "/tmp/a.png");
    REQUIRE(failures.front().description == "model crashed");
    REQUIRE(failures.front().status_msg.find("model crashed") !=
            std::string::npos);
    REQUIRE(svc.empty());
}

TEST_CASE("poll: out vectors accumulate across multiple polls",
          "[sr_task_service]") {
    SrTaskService svc;
    auto fake_a = std::make_unique<FakeEngine>();
    auto* fake_a_raw = fake_a.get();
    auto factory_a = [&]() -> std::unique_ptr<SRInferEngine> {
        return std::move(fake_a);
    };
    auto fake_b = std::make_unique<FakeEngine>();
    auto* fake_b_raw = fake_b.get();
    fake_b_raw->output_path = "/tmp/b_sr.png";
    auto factory_b = [&]() -> std::unique_ptr<SRInferEngine> {
        return std::move(fake_b);
    };
    REQUIRE(svc.start(make_params("a"), factory_a).ok);
    REQUIRE(svc.start(make_params("b"), factory_b).ok);

    std::vector<SrCompletion> completions;
    std::vector<SrFailure> failures;

    fake_a_raw->status = SREngineStatus::Failed;
    svc.poll(completions, failures);
    REQUIRE(completions.empty());
    REQUIRE(failures.size() == 1);
    REQUIRE(svc.size() == 1);

    fake_b_raw->status = SREngineStatus::Completed;
    svc.poll(completions, failures);
    REQUIRE(completions.size() == 1);
    REQUIRE(failures.size() == 1);
    REQUIRE(svc.empty());
}

TEST_CASE("running_count reflects engine status", "[sr_task_service]") {
    SrTaskService svc;
    auto fake_a = std::make_unique<FakeEngine>();
    auto* fake_a_raw = fake_a.get();
    auto factory_a = [&]() -> std::unique_ptr<SRInferEngine> {
        return std::move(fake_a);
    };
    auto fake_b = std::make_unique<FakeEngine>();
    auto* fake_b_raw = fake_b.get();
    auto factory_b = [&]() -> std::unique_ptr<SRInferEngine> {
        return std::move(fake_b);
    };
    REQUIRE(svc.start(make_params("a"), factory_a).ok);
    REQUIRE(svc.start(make_params("b"), factory_b).ok);

    REQUIRE(svc.running_count() == 2);
    REQUIRE(svc.has_running());

    fake_a_raw->status = SREngineStatus::Idle;
    REQUIRE(svc.running_count() == 1);
    REQUIRE(svc.has_running());

    fake_b_raw->status = SREngineStatus::Idle;
    REQUIRE(svc.running_count() == 0);
    REQUIRE_FALSE(svc.has_running());
}

TEST_CASE("cancel_all invokes cancel on every engine", "[sr_task_service]") {
    SrTaskService svc;
    auto fake_a = std::make_unique<FakeEngine>();
    auto* fake_a_raw = fake_a.get();
    auto factory_a = [&]() -> std::unique_ptr<SRInferEngine> {
        return std::move(fake_a);
    };
    auto fake_b = std::make_unique<FakeEngine>();
    auto* fake_b_raw = fake_b.get();
    auto factory_b = [&]() -> std::unique_ptr<SRInferEngine> {
        return std::move(fake_b);
    };
    REQUIRE(svc.start(make_params("a"), factory_a).ok);
    REQUIRE(svc.start(make_params("b"), factory_b).ok);

    svc.cancel_all();

    REQUIRE(fake_a_raw->cancel_calls == 1);
    REQUIRE(fake_b_raw->cancel_calls == 1);
}

TEST_CASE("destructor cancels every still-running engine",
          "[sr_task_service]") {
    auto fake = std::make_unique<FakeEngine>();
    int cancel_calls = 0;
    fake->cancel_calls_external = &cancel_calls;
    {
        SrTaskService svc;
        auto factory = [&]() -> std::unique_ptr<SRInferEngine> {
            return std::move(fake);
        };
        REQUIRE(svc.start(make_params("a"), factory).ok);
        REQUIRE(cancel_calls == 0);
    }
    REQUIRE(cancel_calls == 1);
}
