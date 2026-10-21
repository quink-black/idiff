// Tests for ProcessLauncher.  They run /bin/sh and are skipped on Windows,
// where the launcher is a stub.

#include <catch2/catch_test_macros.hpp>

#include "core/process_launcher.h"

#ifndef _WIN32

#include <chrono>
#include <thread>

using idiff::ProcessLauncher;
using idiff::ProcessResult;

namespace {

ProcessResult run_to_end(ProcessLauncher& launcher) {
    for (int i = 0; i < 500; ++i) {
        auto done = launcher.poll();
        if (!done.empty()) return done.front();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    FAIL("process did not finish");
    return {};
}

} // namespace

TEST_CASE("start: reports the exit code", "[process_launcher]") {
    ProcessLauncher launcher;
    std::string error;
    REQUIRE(launcher.start("job", {"/bin/sh", "-c", "exit 3"}, {}, error));
    REQUIRE(launcher.running_count() == 1);

    auto result = run_to_end(launcher);
    REQUIRE(result.name == "job");
    REQUIRE(result.exit_code == 3);
    REQUIRE(launcher.running_count() == 0);
}

TEST_CASE("start: keeps the tail of stderr", "[process_launcher]") {
    ProcessLauncher launcher;
    std::string error;
    REQUIRE(launcher.start(
        "job", {"/bin/sh", "-c", "echo oops >&2; exit 1"}, {}, error));

    auto result = run_to_end(launcher);
    REQUIRE(result.exit_code == 1);
    REQUIRE(result.stderr_tail == "oops\n");
}

TEST_CASE("start: extra environment replaces inherited variables",
          "[process_launcher]") {
    ProcessLauncher launcher;
    std::string error;
    REQUIRE(launcher.start(
        "job",
        {"/bin/sh", "-c", "test \"$IDIFF_TEST_VAR\" = new"},
        {{"IDIFF_TEST_VAR", "new"}}, error));
    REQUIRE(run_to_end(launcher).exit_code == 0);
}

TEST_CASE("start: a signal death maps to 128 plus the signal",
          "[process_launcher]") {
    ProcessLauncher launcher;
    std::string error;
    REQUIRE(launcher.start("job", {"/bin/sh", "-c", "kill -9 $$"}, {}, error));
    REQUIRE(run_to_end(launcher).exit_code == 137);
}

TEST_CASE("start: a missing program is an error and is not tracked",
          "[process_launcher]") {
    ProcessLauncher launcher;
    std::string error;
    REQUIRE_FALSE(launcher.start(
        "job", {"/nonexistent/idiff-no-such-program"}, {}, error));
    REQUIRE_FALSE(error.empty());
    REQUIRE(launcher.running_count() == 0);
}

TEST_CASE("start: an empty command is rejected", "[process_launcher]") {
    ProcessLauncher launcher;
    std::string error;
    REQUIRE_FALSE(launcher.start("job", {}, {}, error));
    REQUIRE_FALSE(launcher.start("job", {""}, {}, error));
}

TEST_CASE("start: output flood does not block the child",
          "[process_launcher]") {
    ProcessLauncher launcher;
    std::string error;
    REQUIRE(launcher.start(
        "job",
        {"/bin/sh", "-c",
         "i=0; while [ $i -lt 4000 ]; do echo xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx >&2; i=$((i+1)); done"},
        {}, error));
    auto result = run_to_end(launcher);
    REQUIRE(result.exit_code == 0);
    REQUIRE(result.stderr_tail.size() <= 512);
}

#else

TEST_CASE("start: unsupported on Windows", "[process_launcher]") {
    idiff::ProcessLauncher launcher;
    std::string error;
    REQUIRE_FALSE(launcher.start("job", {"cmd"}, {}, error));
}

#endif
