#include "util/logger.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace {

namespace ilog = idiff::log;

class CapturingSink : public ilog::ILogSink {
public:
    explicit CapturingSink(ilog::Level min_level = ilog::Level::Trace)
        : min_(min_level) {}

    bool enabled(ilog::Level lvl) const noexcept override {
        return static_cast<int>(lvl) >= static_cast<int>(min_);
    }
    void write(const ilog::Record& r) override {
        std::lock_guard<std::mutex> lk(mtx_);
        records_.push_back(r);
    }
    void flush() override {}

    std::vector<ilog::Record> snapshot() {
        std::lock_guard<std::mutex> lk(mtx_);
        return records_;
    }

private:
    ilog::Level min_;
    std::mutex mtx_;
    std::vector<ilog::Record> records_;
};

// Restore the previous sink at scope exit so tests do not leak state.
struct SinkScope {
    SinkScope(std::unique_ptr<ilog::ILogSink> s) {
        previous_ = ilog::set_sink(std::move(s));
    }
    ~SinkScope() {
        ilog::set_sink(std::move(previous_));
    }
    std::unique_ptr<ilog::ILogSink> previous_;
};

} // namespace

TEST_CASE("logger default sink is no-op", "[logger]") {
    // No SinkScope -- exercise the global default state.
    ilog::set_sink(nullptr);
    auto& s = ilog::sink();
    REQUIRE_FALSE(s.enabled(ilog::Level::Error));
    LOG_ERROR("must not crash even when no sink is installed");
}

TEST_CASE("logger formats arguments and records location", "[logger]") {
    auto sink = std::make_unique<CapturingSink>();
    auto* raw = sink.get();
    SinkScope scope(std::move(sink));

    LOG_INFO("answer is %d", 42);
    auto recs = raw->snapshot();
    REQUIRE(recs.size() == 1);
    REQUIRE(recs[0].level == ilog::Level::Info);
    REQUIRE(recs[0].message == "answer is 42");
    REQUIRE(recs[0].line > 0);
    // __FILE__ should at least mention this test source file.
    REQUIRE(std::string(recs[0].file).find("test_logger") != std::string::npos);
}

TEST_CASE("logger respects level filter", "[logger]") {
    auto sink = std::make_unique<CapturingSink>(ilog::Level::Warn);
    auto* raw = sink.get();
    SinkScope scope(std::move(sink));

    LOG_TRACE("filtered");
    LOG_DEBUG("filtered");
    LOG_INFO("filtered");
    LOG_WARN("kept-1");
    LOG_ERROR("kept-2");

    auto recs = raw->snapshot();
    REQUIRE(recs.size() == 2);
    REQUIRE(recs[0].message == "kept-1");
    REQUIRE(recs[1].message == "kept-2");
}

TEST_CASE("logger short-circuits formatting when disabled", "[logger]") {
    auto sink = std::make_unique<CapturingSink>(ilog::Level::Error);
    SinkScope scope(std::move(sink));

    // If the macro did not short-circuit, this lambda would run and
    // mark the test as failed.  The macro contract guarantees the
    // arguments are not evaluated when enabled() is false.
    int touched = 0;
    auto fmt_arg = [&]() { touched = 1; return 0; };
    LOG_DEBUG("never %d", fmt_arg());
    REQUIRE(touched == 0);
}

TEST_CASE("logger is safe under concurrent writers", "[logger]") {
    auto sink = std::make_unique<CapturingSink>();
    auto* raw = sink.get();
    SinkScope scope(std::move(sink));

    constexpr int kThreads = 4;
    constexpr int kPerThread = 200;
    std::vector<std::thread> ts;
    ts.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([t]() {
            for (int i = 0; i < kPerThread; ++i) {
                LOG_INFO("t%d-i%d", t, i);
            }
        });
    }
    for (auto& th : ts) th.join();

    auto recs = raw->snapshot();
    REQUIRE(recs.size() == static_cast<std::size_t>(kThreads * kPerThread));
}

TEST_CASE("rotating file sink writes and rotates", "[logger][file]") {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "idiff_logger_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    fs::path path = dir / "idiff.log";

    {
        // Tiny rotation threshold so a handful of records trigger it.
        auto sink = std::make_unique<ilog::RotatingFileSink>(
            path.string(), /*max_bytes=*/256, /*max_files=*/3,
            ilog::Level::Trace);
        SinkScope scope(std::move(sink));

        for (int i = 0; i < 20; ++i) {
            LOG_INFO("payload number %d with extra padding text", i);
        }
        ilog::sink().flush();
    }

    REQUIRE(fs::exists(path));
    bool any_backup = fs::exists(dir / "idiff.log.1") ||
                      fs::exists(dir / "idiff.log.2");
    REQUIRE(any_backup);

    // The most recent file must contain at least one record.
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    REQUIRE_FALSE(ss.str().empty());

    fs::remove_all(dir);
}
