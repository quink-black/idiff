// Tests for FileWatcher: cross-platform filesystem change detection.
//
// The watcher runs a background thread with kernel-level notifications
// (kqueue/inotify) or polling.  These tests exercise the public
// contract: add paths, modify files on disk, poll for changes.

#include <catch2/catch_test_macros.hpp>

#include "core/file_watcher.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::string write_tmp_file(const std::string& tag,
                           const std::string& filename,
                           const std::string& content = "hello") {
    auto dir = std::filesystem::temp_directory_path()
             / ("idiff_fw_" + tag);
    std::filesystem::create_directories(dir);
    auto path = dir / filename;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
    out.close();
    return path.string();
}

// Wait for the watcher to report a change, with a timeout.
std::vector<std::string> poll_until(idiff::FileWatcher& fw,
                                    int timeout_ms = 3000) {
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        auto changed = fw.poll_changed();
        if (!changed.empty()) return changed;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return {};
}

} // namespace

TEST_CASE("FileWatcher: poll returns empty when nothing changed",
          "[file_watcher]") {
    idiff::FileWatcher fw;
    auto path = write_tmp_file("no_change", "stable.txt");
    fw.add_path(path);

    // Give the watcher time to register the path
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto changed = fw.poll_changed();
    REQUIRE(changed.empty());
}

TEST_CASE("FileWatcher: detects in-place write",
          "[file_watcher]") {
    auto path = write_tmp_file("inplace", "target.txt", "original");

    idiff::FileWatcher fw;
    fw.add_path(path);

    // Wait for the watcher thread to register
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Modify the file in place
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "modified content that is longer";
    }

    auto changed = poll_until(fw);
    REQUIRE_FALSE(changed.empty());
    REQUIRE(changed[0] == path);
}

TEST_CASE("FileWatcher: detects mv replacement (rename over existing path)",
          "[file_watcher]") {
    auto path = write_tmp_file("mv_replace", "watched.txt", "old");

    idiff::FileWatcher fw;
    fw.add_path(path);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Simulate `mv new.txt watched.txt` by writing a temp file and
    // renaming it over the watched path.
    auto dir = std::filesystem::path(path).parent_path();
    auto tmp_path = dir / "new_file.txt";
    {
        std::ofstream out(tmp_path, std::ios::binary);
        out << "new content via rename";
    }
    std::filesystem::rename(tmp_path, path);

    auto changed = poll_until(fw);
    REQUIRE_FALSE(changed.empty());
    REQUIRE(changed[0] == path);
}

TEST_CASE("FileWatcher: poll drains events (second poll is empty)",
          "[file_watcher]") {
    auto path = write_tmp_file("drain", "file.txt", "a");

    idiff::FileWatcher fw;
    fw.add_path(path);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "b";
    }

    auto first = poll_until(fw);
    REQUIRE_FALSE(first.empty());

    // Second poll without any new modification should be empty.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto second = fw.poll_changed();
    REQUIRE(second.empty());
}

TEST_CASE("FileWatcher: remove_path stops notifications",
          "[file_watcher]") {
    auto path = write_tmp_file("remove", "gone.txt", "x");

    idiff::FileWatcher fw;
    fw.add_path(path);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    fw.remove_path(path);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Modify after removal -- should not be reported.
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "modified after unwatched";
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    auto changed = fw.poll_changed();
    REQUIRE(changed.empty());
}

TEST_CASE("FileWatcher: clear removes all watches",
          "[file_watcher]") {
    auto p1 = write_tmp_file("clear", "a.txt", "1");
    auto p2 = write_tmp_file("clear", "b.txt", "2");

    idiff::FileWatcher fw;
    fw.add_path(p1);
    fw.add_path(p2);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    fw.clear();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    {
        std::ofstream out(p1, std::ios::binary | std::ios::trunc);
        out << "changed";
    }
    {
        std::ofstream out(p2, std::ios::binary | std::ios::trunc);
        out << "changed";
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    auto changed = fw.poll_changed();
    REQUIRE(changed.empty());
}

TEST_CASE("FileWatcher: duplicate add_path is a no-op",
          "[file_watcher]") {
    auto path = write_tmp_file("dup_add", "same.txt", "data");

    idiff::FileWatcher fw;
    fw.add_path(path);
    fw.add_path(path); // duplicate

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "new data";
    }

    auto changed = poll_until(fw);
    // Should report the path exactly once, not twice.
    REQUIRE(changed.size() == 1);
    REQUIRE(changed[0] == path);
}

TEST_CASE("FileWatcher: non-existent path is silently skipped",
          "[file_watcher]") {
    idiff::FileWatcher fw;
    fw.add_path("/no/such/path/ever.txt");

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto changed = fw.poll_changed();
    REQUIRE(changed.empty());
}
