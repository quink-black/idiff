#include "core/file_watcher.h"

#include "util/logger.h"

#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#if defined(__APPLE__) || defined(__FreeBSD__)
#define IDIFF_USE_KQUEUE 1
#include <fcntl.h>
#include <sys/event.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#elif defined(__linux__)
#define IDIFF_USE_INOTIFY 1
#include <fcntl.h>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>
#elif defined(_WIN32)
#define IDIFF_USE_RDCW 1
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <memory>
#else
#define IDIFF_USE_POLLING 1
#include <chrono>
#include <condition_variable>
#include <filesystem>
#endif

namespace idiff {

// --------------------------------------------------------------------------
// kqueue backend (macOS, FreeBSD)
// --------------------------------------------------------------------------
#if defined(IDIFF_USE_KQUEUE)

struct FileWatcher::Impl {
    std::mutex mutex_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    int kq_ = -1;

    // pipe used to wake the kqueue thread when paths are added/removed
    int wake_pipe_[2] = {-1, -1};

    // path -> fd being watched.  We deliberately do NOT track mtime /
    // size here: kqueue (with NOTE_ATTRIB filtered out at registration
    // time) only fires for genuine content/structure changes, and a
    // stat()-based second pass would race with the writer on small
    // edits where mtime granularity hides the change.
    std::unordered_map<std::string, int> watched_;
    // fd -> path (reverse lookup for kevent results)
    std::unordered_map<int, std::string> fd_to_path_;

    // Changed paths accumulated between polls
    std::unordered_set<std::string> changed_;

    Impl() {
        kq_ = kqueue();
        if (kq_ < 0) {
            LOG_ERROR("FileWatcher: kqueue() failed");
            return;
        }
        if (pipe(wake_pipe_) != 0) {
            LOG_ERROR("FileWatcher: pipe() failed");
            close(kq_);
            kq_ = -1;
            return;
        }
        // Make both ends non-blocking so drain_wake_pipe() never stalls
        // and wake() never blocks if the pipe buffer fills.
        fcntl(wake_pipe_[0], F_SETFL, O_NONBLOCK);
        fcntl(wake_pipe_[1], F_SETFL, O_NONBLOCK);

        // Register the read end of the pipe with kqueue so we can
        // interrupt the blocking kevent() call.
        struct kevent ev;
        EV_SET(&ev, wake_pipe_[0], EVFILT_READ, EV_ADD, 0, 0, nullptr);
        kevent(kq_, &ev, 1, nullptr, 0, nullptr);

        running_.store(true);
        thread_ = std::thread([this] { run(); });
    }

    ~Impl() {
        running_.store(false);
        wake();
        if (thread_.joinable()) thread_.join();

        for (auto& [path, fd] : watched_) {
            close(fd);
        }
        if (kq_ >= 0) close(kq_);
        if (wake_pipe_[0] >= 0) close(wake_pipe_[0]);
        if (wake_pipe_[1] >= 0) close(wake_pipe_[1]);
    }

    void wake() {
        if (wake_pipe_[1] >= 0) {
            char c = 'w';
            (void)write(wake_pipe_[1], &c, 1);
        }
    }

    void drain_wake_pipe() {
        char buf[64];
        while (read(wake_pipe_[0], buf, sizeof(buf)) > 0) {}
    }

    void add_path(const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (watched_.count(path)) return;

        int fd = open(path.c_str(), O_EVTONLY);
        if (fd < 0) {
            LOG_WARN("FileWatcher: cannot open '%s' for watching", path.c_str());
            return;
        }

        // Watch only "real" content/structure changes.  NOTE_ATTRIB is
        // intentionally omitted because macOS fires it for many things
        // unrelated to user edits (Spotlight metadata, atime updates,
        // Finder "last used" timestamps, antivirus stat() probes), and
        // those would otherwise surface as bogus reload prompts.
        struct kevent ev;
        EV_SET(&ev, fd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
               NOTE_WRITE | NOTE_EXTEND | NOTE_RENAME | NOTE_DELETE,
               0, nullptr);
        if (kevent(kq_, &ev, 1, nullptr, 0, nullptr) < 0) {
            LOG_WARN("FileWatcher: kevent register failed for '%s'", path.c_str());
            close(fd);
            return;
        }

        watched_[path] = fd;
        fd_to_path_[fd] = path;
        wake();
    }

    void remove_path(const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = watched_.find(path);
        if (it == watched_.end()) return;

        int fd = it->second;
        // EV_DELETE is automatic when the fd is closed
        close(fd);
        fd_to_path_.erase(fd);
        watched_.erase(it);
        changed_.erase(path);
        wake();
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [path, fd] : watched_) {
            close(fd);
        }
        watched_.clear();
        fd_to_path_.clear();
        changed_.clear();
        wake();
    }

    std::vector<std::string> poll_changed() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> result(changed_.begin(), changed_.end());
        changed_.clear();
        return result;
    }

    // Re-open the fd for a path whose inode was deleted or renamed.
    // Caller must hold mutex_.  Takes path by value because the
    // caller passes a reference into fd_to_path_ which is erased below.
    void rewatch(std::string path, int old_fd) {
        close(old_fd);
        fd_to_path_.erase(old_fd);

        int new_fd = open(path.c_str(), O_EVTONLY);
        if (new_fd < 0) {
            watched_.erase(path);
            return;
        }
        struct kevent ev;
        EV_SET(&ev, new_fd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
               NOTE_WRITE | NOTE_EXTEND | NOTE_RENAME | NOTE_DELETE,
               0, nullptr);
        kevent(kq_, &ev, 1, nullptr, 0, nullptr);
        watched_[path] = new_fd;
        fd_to_path_[new_fd] = std::move(path);
    }

    void handle_event(const struct kevent& ev) {
        int fd = static_cast<int>(ev.ident);
        if (fd == wake_pipe_[0]) {
            drain_wake_pipe();
            return;
        }
        auto it = fd_to_path_.find(fd);
        if (it == fd_to_path_.end()) return;

        changed_.insert(it->second);

        if (ev.fflags & (NOTE_DELETE | NOTE_RENAME)) {
            rewatch(it->second, fd);
        }
    }

    void run() {
        struct kevent events[16];
        while (running_.load()) {
            struct timespec timeout = {0, 500000000};
            int n = kevent(kq_, nullptr, 0, events, 16, &timeout);
            if (n <= 0) continue;

            std::lock_guard<std::mutex> lock(mutex_);
            for (int i = 0; i < n; ++i) {
                handle_event(events[i]);
            }
        }
    }
};

// --------------------------------------------------------------------------
// inotify backend (Linux)
// --------------------------------------------------------------------------
#elif defined(IDIFF_USE_INOTIFY)

struct FileWatcher::Impl {
    std::mutex mutex_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    int inotify_fd_ = -1;
    int wake_pipe_[2] = {-1, -1};

    // path -> watch descriptor.  We rely on inotify's event mask alone
    // (with IN_ATTRIB filtered out at registration time) to decide
    // whether to report a change, rather than running a stat() second
    // pass that would race with small writes.
    std::unordered_map<std::string, int> watched_;
    // wd -> path
    std::unordered_map<int, std::string> wd_to_path_;

    std::unordered_set<std::string> changed_;

    Impl() {
        inotify_fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (inotify_fd_ < 0) {
            LOG_ERROR("FileWatcher: inotify_init1 failed");
            return;
        }
        if (pipe(wake_pipe_) != 0) {
            LOG_ERROR("FileWatcher: pipe() failed");
            close(inotify_fd_);
            inotify_fd_ = -1;
            return;
        }
        // Non-blocking so the drain loop never stalls and wake()
        // never blocks if the pipe buffer fills.
        fcntl(wake_pipe_[0], F_SETFL, O_NONBLOCK);
        fcntl(wake_pipe_[1], F_SETFL, O_NONBLOCK);

        running_.store(true);
        thread_ = std::thread([this] { run(); });
    }

    ~Impl() {
        running_.store(false);
        wake();
        if (thread_.joinable()) thread_.join();

        for (auto& [path, wd] : watched_) {
            inotify_rm_watch(inotify_fd_, wd);
        }
        if (inotify_fd_ >= 0) close(inotify_fd_);
        if (wake_pipe_[0] >= 0) close(wake_pipe_[0]);
        if (wake_pipe_[1] >= 0) close(wake_pipe_[1]);
    }

    void wake() {
        if (wake_pipe_[1] >= 0) {
            char c = 'w';
            (void)write(wake_pipe_[1], &c, 1);
        }
    }

    void add_path(const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (watched_.count(path)) return;

        // Watch the file directly (inotify supports this; the watch is
        // removed on delete and we re-add on IN_DELETE_SELF below).
        //
        // IN_ATTRIB is intentionally omitted: it fires on chmod, atime
        // updates from antivirus / indexing tools and other things
        // unrelated to user edits, which would translate into a bogus
        // "file modified, reload?" prompt.
        int wd = inotify_add_watch(inotify_fd_, path.c_str(),
                                   IN_MODIFY | IN_CLOSE_WRITE |
                                   IN_DELETE_SELF | IN_MOVE_SELF);
        if (wd < 0) {
            LOG_WARN("FileWatcher: inotify_add_watch failed for '%s'",
                     path.c_str());
            return;
        }
        watched_[path] = wd;
        wd_to_path_[wd] = path;
        wake();
    }

    void remove_path(const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = watched_.find(path);
        if (it == watched_.end()) return;

        inotify_rm_watch(inotify_fd_, it->second);
        wd_to_path_.erase(it->second);
        watched_.erase(it);
        changed_.erase(path);
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [path, wd] : watched_) {
            inotify_rm_watch(inotify_fd_, wd);
        }
        watched_.clear();
        wd_to_path_.clear();
        changed_.clear();
    }

    std::vector<std::string> poll_changed() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> result(changed_.begin(), changed_.end());
        changed_.clear();
        return result;
    }

    // Try to re-watch a path after its inode was deleted or moved.
    // Caller must hold mutex_.  Takes path by value because the
    // caller passes a reference into wd_to_path_ which is erased below.
    void rewatch(std::string path, int old_wd) {
        wd_to_path_.erase(old_wd);
        watched_.erase(path);

        int new_wd = inotify_add_watch(
            inotify_fd_, path.c_str(),
            IN_MODIFY | IN_CLOSE_WRITE |
            IN_DELETE_SELF | IN_MOVE_SELF);
        if (new_wd >= 0) {
            watched_[path] = new_wd;
            wd_to_path_[new_wd] = std::move(path);
        }
    }

    void drain_events(char* buf, ssize_t len) {
        for (char* ptr = buf; ptr < buf + len; ) {
            auto* event = reinterpret_cast<struct inotify_event*>(ptr);
            ptr += sizeof(struct inotify_event) + event->len;

            auto it = wd_to_path_.find(event->wd);
            if (it == wd_to_path_.end()) continue;

            changed_.insert(it->second);

            if (event->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) {
                rewatch(it->second, event->wd);
            }
        }
    }

    void run() {
        struct pollfd fds[2];
        fds[0].fd = inotify_fd_;
        fds[0].events = POLLIN;
        fds[1].fd = wake_pipe_[0];
        fds[1].events = POLLIN;

        alignas(struct inotify_event) char buf[4096];

        while (running_.load()) {
            int ret = poll(fds, 2, 500);
            if (ret <= 0) continue;

            if (fds[1].revents & POLLIN) {
                char tmp[64];
                while (read(wake_pipe_[0], tmp, sizeof(tmp)) > 0) {}
            }
            if (!(fds[0].revents & POLLIN)) continue;

            ssize_t len = read(inotify_fd_, buf, sizeof(buf));
            if (len <= 0) continue;

            std::lock_guard<std::mutex> lock(mutex_);
            drain_events(buf, len);
        }
    }
};

// --------------------------------------------------------------------------
// ReadDirectoryChangesW + IOCP backend (Windows)
// --------------------------------------------------------------------------
#elif defined(IDIFF_USE_RDCW)

namespace {

// Lowercase a UTF-16 string for case-insensitive filename comparison.
// Windows filesystems are case-preserving but case-insensitive on the
// default NTFS volumes used by the test suite.
std::wstring to_lower_w(std::wstring w) {
    std::transform(w.begin(), w.end(), w.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
    return w;
}

} // namespace

struct FileWatcher::Impl {
    // ----- Per-directory watch state ------------------------------------
    // One DirWatch covers all watched files that live in the same parent
    // directory.  This minimises the number of HANDLEs and outstanding
    // ReadDirectoryChangesW calls.
    struct DirWatch {
        std::wstring dir_w;                 // canonical wide path
        HANDLE handle = INVALID_HANDLE_VALUE;
        OVERLAPPED overlapped{};
        // Buffer must be DWORD-aligned and large enough for a burst of
        // changes.  64 KiB is the documented maximum for remote shares;
        // we pick 32 KiB which is plenty for local NTFS.
        alignas(DWORD) BYTE buffer[32 * 1024]{};
        // Per-watched-file metadata.  We key by lowercase wide name to
        // match Windows' case-insensitive semantics; the value carries
        // the original UTF-8 path the caller registered as well as a
        // short cooldown timestamp used to coalesce the secondary
        // metadata-flush event ReadDirectoryChangesW emits a few tens
        // of milliseconds after a write+close (see decode_notifications).
        struct FileEntry {
            std::string original_path;
            std::chrono::steady_clock::time_point cooldown_until{};
        };
        std::unordered_map<std::wstring, FileEntry> files;
        bool pending = false;               // is an async read outstanding?
    };

    std::mutex mutex_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    HANDLE iocp_ = nullptr;

    // Special completion key used by wake() to interrupt the worker.
    // Real ReadDirectoryChangesW completions carry a DirWatch* as the
    // key, which is always a valid pointer and never collides with this
    // sentinel value.
    static constexpr ULONG_PTR KEY_WAKE = 1;

    // dir_lower_wide -> DirWatch
    std::unordered_map<std::wstring, std::unique_ptr<DirWatch>> dirs_;
    // path (as passed by caller) -> dir_lower_wide so remove_path can
    // find which DirWatch hosts it.
    std::unordered_map<std::string, std::wstring> path_to_dir_;

    std::unordered_set<std::string> changed_;

    Impl() {
        iocp_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
        if (!iocp_) {
            LOG_ERROR("FileWatcher: CreateIoCompletionPort failed (err=%lu)",
                      GetLastError());
            return;
        }
        running_.store(true);
        thread_ = std::thread([this] { run(); });
    }

    ~Impl() {
        running_.store(false);
        wake();
        if (thread_.joinable()) thread_.join();

        // Cancel and close every directory handle.  This must happen on
        // the same thread that no longer pumps the IOCP, after the worker
        // has joined, so leftover completions are simply discarded.
        for (auto& [_, d] : dirs_) {
            if (d->handle != INVALID_HANDLE_VALUE) {
                CancelIoEx(d->handle, &d->overlapped);
                CloseHandle(d->handle);
            }
        }
        dirs_.clear();
        if (iocp_) CloseHandle(iocp_);
    }

    void wake() {
        if (iocp_) {
            PostQueuedCompletionStatus(iocp_, 0, KEY_WAKE, nullptr);
        }
    }

    // Open a directory handle suitable for ReadDirectoryChangesW and
    // associate it with the IOCP.  Returns nullptr on failure.
    std::unique_ptr<DirWatch> open_dir(const std::wstring& dir_w) {
        HANDLE h = CreateFileW(
            dir_w.c_str(),
            FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
            nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            LOG_WARN("FileWatcher: CreateFileW failed for directory (err=%lu)",
                     GetLastError());
            return nullptr;
        }
        auto d = std::make_unique<DirWatch>();
        d->dir_w = dir_w;
        d->handle = h;

        // Associate handle with our IOCP.  Use the DirWatch pointer as the
        // completion key so the worker can recover it from the completion.
        if (!CreateIoCompletionPort(h, iocp_,
                                    reinterpret_cast<ULONG_PTR>(d.get()),
                                    0)) {
            LOG_WARN("FileWatcher: associate IOCP failed (err=%lu)",
                     GetLastError());
            CloseHandle(h);
            return nullptr;
        }
        return d;
    }

    // Issue a fresh asynchronous ReadDirectoryChangesW.  Caller must
    // ensure no read is currently outstanding.
    bool issue_read(DirWatch* d) {
        ZeroMemory(&d->overlapped, sizeof(OVERLAPPED));
        // FILE_NOTIFY_CHANGE_ATTRIBUTES is intentionally omitted: it
        // fires for ACL edits, hidden/system bit toggles, archive bit
        // resets, and so on, none of which represent user-visible
        // content changes and would otherwise translate into bogus
        // reload prompts on Windows.
        DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME
                     | FILE_NOTIFY_CHANGE_LAST_WRITE
                     | FILE_NOTIFY_CHANGE_SIZE
                     | FILE_NOTIFY_CHANGE_CREATION;
        BOOL ok = ReadDirectoryChangesW(
            d->handle,
            d->buffer,
            sizeof(d->buffer),
            FALSE,                  // bWatchSubtree: we only need this dir
            filter,
            nullptr,                // lpBytesReturned (async => unused)
            &d->overlapped,
            nullptr);
        if (!ok) {
            LOG_WARN("FileWatcher: ReadDirectoryChangesW failed (err=%lu)",
                     GetLastError());
            d->pending = false;
            return false;
        }
        d->pending = true;
        return true;
    }

    void add_path(const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (path_to_dir_.count(path)) return;       // duplicate

        std::error_code ec;
        std::filesystem::path fs_path(path);
        // weakly_canonical resolves "." / ".." and converts to absolute.
        // If the path does not exist we silently skip, matching the
        // behaviour of the POSIX backends.
        auto canonical = std::filesystem::weakly_canonical(fs_path, ec);
        if (ec) canonical = fs_path;
        if (!std::filesystem::exists(canonical, ec)) {
            LOG_WARN("FileWatcher: path '%s' does not exist", path.c_str());
            return;
        }
        auto parent = canonical.parent_path();
        auto filename = canonical.filename();
        if (parent.empty() || filename.empty()) {
            LOG_WARN("FileWatcher: path '%s' has no parent directory",
                     path.c_str());
            return;
        }

        std::wstring dir_w = parent.wstring();
        std::wstring file_w = filename.wstring();
        std::wstring dir_key = to_lower_w(dir_w);
        std::wstring file_key = to_lower_w(file_w);

        auto it = dirs_.find(dir_key);
        if (it == dirs_.end()) {
            auto d = open_dir(dir_w);
            if (!d) return;
            DirWatch* raw = d.get();
            dirs_.emplace(dir_key, std::move(d));
            it = dirs_.find(dir_key);
            // Issue first read; it will land on the worker thread once a
            // change occurs or we cancel during shutdown.
            if (!issue_read(raw)) {
                CloseHandle(raw->handle);
                dirs_.erase(it);
                return;
            }
        }
        it->second->files[file_key] = DirWatch::FileEntry{path, {}};
        path_to_dir_[path] = dir_key;
    }

    void remove_path(const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto pit = path_to_dir_.find(path);
        if (pit == path_to_dir_.end()) return;

        auto dit = dirs_.find(pit->second);
        if (dit != dirs_.end()) {
            // Erase by recomputing the lowercase filename key.
            std::filesystem::path fs_path(path);
            std::wstring file_key = to_lower_w(fs_path.filename().wstring());
            dit->second->files.erase(file_key);
            if (dit->second->files.empty()) {
                close_dir_locked(dit->second.get());
                dirs_.erase(dit);
            }
        }
        path_to_dir_.erase(pit);
        changed_.erase(path);
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [_, d] : dirs_) {
            close_dir_locked(d.get());
        }
        dirs_.clear();
        path_to_dir_.clear();
        changed_.clear();
    }

    // Caller holds mutex_.  CancelIoEx + CloseHandle drains any pending
    // ReadDirectoryChangesW; the worker will observe the completion with
    // ERROR_OPERATION_ABORTED and ignore it because pending becomes false.
    void close_dir_locked(DirWatch* d) {
        if (d->handle != INVALID_HANDLE_VALUE) {
            d->pending = false;
            CancelIoEx(d->handle, &d->overlapped);
            CloseHandle(d->handle);
            d->handle = INVALID_HANDLE_VALUE;
        }
    }

    std::vector<std::string> poll_changed() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> result(changed_.begin(), changed_.end());
        changed_.clear();

        // Arm a brief cooldown for every path we just reported.  NTFS
        // emits a secondary metadata-flush notification a few tens of
        // milliseconds after a write+close, which would otherwise show
        // up as a spurious second event for the same edit.  150 ms is
        // long enough to absorb that flush yet far shorter than any
        // realistic interval between two distinct user edits.
        const auto cooldown = std::chrono::steady_clock::now()
                            + std::chrono::milliseconds(150);
        for (const auto& p : result) {
            auto pit = path_to_dir_.find(p);
            if (pit == path_to_dir_.end()) continue;
            auto dit = dirs_.find(pit->second);
            if (dit == dirs_.end()) continue;
            std::filesystem::path fs_path(p);
            std::wstring file_key = to_lower_w(fs_path.filename().wstring());
            auto fit = dit->second->files.find(file_key);
            if (fit != dit->second->files.end()) {
                fit->second.cooldown_until = cooldown;
            }
        }
        return result;
    }

    // Decode one filled buffer of FILE_NOTIFY_INFORMATION records.
    // Caller holds mutex_.
    void decode_notifications(DirWatch* d, DWORD bytes) {
        const auto now = std::chrono::steady_clock::now();
        if (bytes == 0) {
            // A zero-byte completion means the buffer overflowed.  Mark
            // every watched file in this directory as changed because we
            // cannot know which entries were affected -- skipping those
            // still in their post-poll cooldown so we do not double-
            // report a write that already completed.
            for (auto& [_, fe] : d->files) {
                if (now < fe.cooldown_until) continue;
                changed_.insert(fe.original_path);
            }
            return;
        }
        BYTE* p = d->buffer;
        BYTE* end = d->buffer + bytes;
        while (p + sizeof(FILE_NOTIFY_INFORMATION) <= end) {
            auto* info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(p);
            size_t name_len = info->FileNameLength / sizeof(WCHAR);
            std::wstring name(info->FileName, name_len);
            std::wstring key = to_lower_w(name);

            auto fit = d->files.find(key);
            if (fit != d->files.end()) {
                if (now >= fit->second.cooldown_until) {
                    changed_.insert(fit->second.original_path);
                }
            }
            if (info->NextEntryOffset == 0) break;
            p += info->NextEntryOffset;
        }
    }

    void run() {
        while (running_.load()) {
            DWORD bytes = 0;
            ULONG_PTR key = 0;
            OVERLAPPED* ov = nullptr;
            BOOL ok = GetQueuedCompletionStatus(iocp_, &bytes, &key, &ov,
                                                INFINITE);
            if (!running_.load()) break;

            if (key == KEY_WAKE) {
                continue;                           // just a wake-up
            }
            // key == reinterpret_cast<ULONG_PTR>(DirWatch*)
            DirWatch* d = reinterpret_cast<DirWatch*>(key);

            // Validate that this DirWatch is still alive.  After clear()
            // or remove_path() the entry may have been freed; in that
            // case the completion belongs to a CancelIoEx and we drop it.
            std::lock_guard<std::mutex> lock(mutex_);
            bool alive = false;
            for (auto& [_, p] : dirs_) {
                if (p.get() == d) { alive = true; break; }
            }
            if (!alive) continue;
            if (d->handle == INVALID_HANDLE_VALUE) continue;

            d->pending = false;

            if (!ok) {
                DWORD err = GetLastError();
                if (err == ERROR_OPERATION_ABORTED) continue;
                LOG_WARN("FileWatcher: completion error %lu", err);
                // Re-arm and hope for the best.
                issue_read(d);
                continue;
            }
            decode_notifications(d, bytes);
            // Re-arm immediately so we never miss subsequent changes.
            issue_read(d);
        }
    }
};

// --------------------------------------------------------------------------
// Polling fallback (other platforms)
// --------------------------------------------------------------------------
#else

struct FileWatcher::Impl {
    std::mutex mutex_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::condition_variable cv_;

    struct WatchedFile {
        std::string path;
        std::time_t last_mtime = 0;
        off_t last_size = 0;
    };
    std::unordered_map<std::string, WatchedFile> watched_;
    std::unordered_set<std::string> changed_;

    static std::time_t get_mtime(const std::string& path) {
        struct stat st;
        if (stat(path.c_str(), &st) != 0) return 0;
        return st.st_mtime;
    }

    static off_t get_size(const std::string& path) {
        struct stat st;
        if (stat(path.c_str(), &st) != 0) return -1;
        return st.st_size;
    }

    Impl() {
        running_.store(true);
        thread_ = std::thread([this] { run(); });
    }

    ~Impl() {
        running_.store(false);
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

    void add_path(const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (watched_.count(path)) return;
        WatchedFile wf;
        wf.path = path;
        wf.last_mtime = get_mtime(path);
        wf.last_size = get_size(path);
        if (wf.last_mtime == 0) {
            LOG_WARN("FileWatcher: cannot stat '%s' for watching", path.c_str());
            return;
        }
        watched_[path] = wf;
    }

    void remove_path(const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex_);
        watched_.erase(path);
        changed_.erase(path);
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        watched_.clear();
        changed_.clear();
    }

    std::vector<std::string> poll_changed() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> result(changed_.begin(), changed_.end());
        changed_.clear();
        return result;
    }

    void run() {
        while (running_.load()) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(500),
                             [this] { return !running_.load(); });
                if (!running_.load()) break;

                for (auto& [path, wf] : watched_) {
                    auto mtime = get_mtime(path);
                    auto size = get_size(path);
                    if (mtime != wf.last_mtime || size != wf.last_size) {
                        wf.last_mtime = mtime;
                        wf.last_size = size;
                        changed_.insert(path);
                    }
                }
            }
        }
    }
};

#endif

// --------------------------------------------------------------------------
// FileWatcher public interface (delegates to Impl)
// --------------------------------------------------------------------------

FileWatcher::FileWatcher() : impl_(std::make_unique<Impl>()) {}

FileWatcher::~FileWatcher() = default;

FileWatcher::FileWatcher(FileWatcher&&) noexcept = default;
FileWatcher& FileWatcher::operator=(FileWatcher&&) noexcept = default;

void FileWatcher::add_path(const std::string& path) {
    impl_->add_path(path);
}

void FileWatcher::remove_path(const std::string& path) {
    impl_->remove_path(path);
}

void FileWatcher::clear() {
    impl_->clear();
}

std::vector<std::string> FileWatcher::poll_changed() {
    return impl_->poll_changed();
}

} // namespace idiff