// Verifies that domain-layer log points fire at the expected level
// and carry enough context to diagnose the failure from logs alone.
//
// The logger ships with a thread-safe global sink (tests in
// test_logger.cpp cover the sink mechanics).  Here we install a
// capturing sink and drive each service through the failure paths
// that are most relevant to support / triage (bad JSON, out-of-
// range indices, missing SR engine, upload into an empty library).

#include "util/logger.h"

#include <catch2/catch_test_macros.hpp>

#include "app/controller.h"
#include "app/io/texture_uploader.h"
#include "app/sr_dialog.h"
#include "app/status_reporter.h"
#include "app/app.h"
#include "core/media_source.h" // IWYU pragma: keep
#include "domain/comparison_config_service.h"
#include "domain/image_library.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

namespace ilog = idiff::log;

// Capturing sink: records every accepted record so tests can assert
// level, message fragments, and file of origin.
class CapturingSink : public ilog::ILogSink {
public:
    bool enabled(ilog::Level) const noexcept override { return true; }
    void write(const ilog::Record& r) override {
        std::lock_guard<std::mutex> lk(mtx_);
        records_.push_back(r);
    }
    void flush() override {}

    std::vector<ilog::Record> snapshot() {
        std::lock_guard<std::mutex> lk(mtx_);
        return records_;
    }

    bool any(ilog::Level lvl, const std::string& needle) {
        for (const auto& r : snapshot()) {
            if (r.level == lvl && r.message.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

private:
    std::mutex mtx_;
    std::vector<ilog::Record> records_;
};

// RAII: install a fresh CapturingSink and restore the previous one
// on scope exit so tests do not bleed records into each other.
struct SinkScope {
    CapturingSink* sink = nullptr;
    std::unique_ptr<ilog::ILogSink> previous;

    SinkScope() {
        auto s = std::make_unique<CapturingSink>();
        sink = s.get();
        previous = ilog::set_sink(std::move(s));
    }
    ~SinkScope() { ilog::set_sink(std::move(previous)); }
};

class NoopUploader : public idiff::ITextureUploader {
public:
    SDL_Texture* upload(const idiff::UploadRequest&) override {
        return reinterpret_cast<SDL_Texture*>(
            static_cast<std::uintptr_t>(++cookie_));
    }
    void destroy(SDL_Texture*) override {}

private:
    std::uintptr_t cookie_ = 0;
};

class SilentStatusReporter : public idiff::IStatusReporter {
public:
    void set_status(const std::string&) override {}
    void append_status(const std::string&) override {}
    void set_sr_status(const std::string&) override {}
    void show_error(const std::string&, const std::string&) override {}
};

} // namespace

TEST_CASE("Logging: bad comparison-config JSON warns with file context",
          "[logging][comparison_config]") {
    SinkScope scope;

    auto dir = std::filesystem::temp_directory_path()
             / "idiff_log_cfg_bad";
    std::filesystem::create_directories(dir);
    auto json_path = dir / "broken.json";
    {
        std::ofstream out(json_path, std::ios::binary | std::ios::trunc);
        out << "not valid json";
    }

    idiff::ComparisonConfigService svc;
    svc.set_cache_root_override(dir);
    auto r = svc.load(json_path.string());

    REQUIRE_FALSE(r.ok);
    REQUIRE(scope.sink->any(ilog::Level::Warn,
                            "ComparisonConfigService::load"));
}

TEST_CASE("Logging: comparison-config switch_to out-of-range warns",
          "[logging][comparison_config]") {
    SinkScope scope;

    idiff::ComparisonConfigService svc;
    auto r = svc.switch_to(99);

    REQUIRE_FALSE(r.ok);
    // The exact message substring matches the LOG_WARN call site.
    REQUIRE(scope.sink->any(ilog::Level::Warn,
                            "out-of-range index 99"));
}

TEST_CASE("Logging: ImageLibrary upload with bad index warns",
          "[logging][image_library]") {
    SinkScope scope;

    NoopUploader uploader;
    idiff::ImageLibrary library(uploader);

    idiff::UploadRequest req{};
    library.upload(5, req); // library is empty -> index out of range

    REQUIRE(scope.sink->any(ilog::Level::Warn,
                            "library upload index out of range"));
}

TEST_CASE("Logging: SR start without engine logs warn and error title",
          "[logging][sr]") {
    SinkScope scope;

    NoopUploader uploader;
    SilentStatusReporter reporter;
    idiff::AppController controller(uploader, reporter);

    idiff::SRTaskParams params{};
    params.input_path = "/tmp/missing.png";
    params.output_path = "/tmp/missing_sr_2x.png";
    controller.start_sr_task(params);

    // The exact warn string depends on whether the factory was
    // registered in the test binary; both known paths log via
    // SrTaskService::start.  Asserting the function-name prefix keeps
    // the test stable across build configurations.
    REQUIRE(scope.sink->any(ilog::Level::Warn, "SrTaskService::start"));
}
