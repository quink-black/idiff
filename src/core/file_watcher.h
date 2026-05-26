#ifndef IDIFF_CORE_FILE_WATCHER_H
#define IDIFF_CORE_FILE_WATCHER_H

// Cross-platform filesystem change monitor.
//
// Tracks a set of file paths and detects when they are modified,
// replaced, or deleted on disk.  Runs a background thread that uses
// kernel-level notifications where available (kqueue on macOS/BSD,
// inotify on Linux) and falls back to stat-based polling elsewhere.
//
// Threading: add_path / remove_path / poll_changed are safe to call
// from any single thread (the "owner" thread, typically the UI
// thread).  The background thread only touches internal state
// protected by a mutex.

#include <memory>
#include <string>
#include <vector>

namespace idiff {

class FileWatcher {
public:
    FileWatcher();
    ~FileWatcher();

    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;
    FileWatcher(FileWatcher&&) noexcept;
    FileWatcher& operator=(FileWatcher&&) noexcept;

    // Begin watching `path`.  Duplicate paths are silently ignored.
    // The path must exist at the time of this call (non-existent
    // paths are skipped with a log warning).
    void add_path(const std::string& path);

    // Stop watching `path`.  No-op if path is not being watched.
    void remove_path(const std::string& path);

    // Remove all watched paths.
    void clear();

    // Drain the set of paths that changed since the last poll.
    // Returns an empty vector when nothing changed.  Each path
    // appears at most once per poll regardless of how many
    // underlying events occurred.
    std::vector<std::string> poll_changed();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace idiff

#endif // IDIFF_CORE_FILE_WATCHER_H
