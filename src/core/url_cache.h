#ifndef IDIFF_URL_CACHE_H
#define IDIFF_URL_CACHE_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace idiff {

// A tiny "download once, reuse forever" file cache.  URLs are mapped to
// a local path whose structure mirrors the URL itself, so a user
// browsing the cache directory can immediately see which file
// corresponds to which URL.
//
// Design notes:
//   * Downloads are delegated to the system `curl` command so the core
//     library has zero new compile/link dependencies.  This is the
//     "minimal HTTP" hook the user asked for.
//   * Memory footprint is zero: curl streams directly to disk, and the
//     cache returns just a filesystem path.  Callers then read the file
//     through the regular image loader when they actually need pixels.
//   * Local filenames are derived deterministically from the URL
//     (scheme-stripped, path-preserving, URL-decoded).  No hashing or
//     random suffix is used so the mapping "cache file -> source URL"
//     stays human-readable.
class UrlCache {
public:
    // Uses the default cache root (see resolve_default_root()).
    UrlCache();

    // Override the cache root.  The directory is created on demand; it
    // is safe to pass a non-existent path.  Typical callers compute a
    // per-config subdirectory (e.g. "Downloads/idiff_cache_foo_YYYYmmdd_HHMMSS")
    // and hand it in here so every config gets its own scoped cache.
    explicit UrlCache(std::filesystem::path root);

    // Shuts down the background worker pool (if any) and joins all
    // threads.  Any prefetches still queued but not yet started are
    // dropped; in-flight curl invocations are allowed to finish so the
    // corresponding `.part` file either becomes a valid cache entry
    // or is cleaned up on failure, never leaving half-written garbage.
    ~UrlCache();

    UrlCache(const UrlCache&) = delete;
    UrlCache& operator=(const UrlCache&) = delete;

    // Resolve the preferred cache root, in priority order:
    //   1. A `cache_root = <path>` (or plain path) line inside
    //      ~/.idiff.config, if present and readable.
    //   2. The platform Downloads directory (e.g. ~/Downloads on
    //      macOS/Linux, %USERPROFILE%\Downloads on Windows).
    //   3. The current working directory as a last resort.
    static std::filesystem::path resolve_default_root();

    // Read `cache_root` from the user config file (~/.idiff.config).
    // Returns an empty path when the file is missing or does not set a
    // cache root.  Exposed separately so callers can distinguish
    // "explicitly configured" from "falling back to Downloads".
    static std::filesystem::path read_user_config_root();

    // Platform-appropriate Downloads directory.  Returns an empty path
    // when it cannot be determined (extremely rare on desktop systems).
    static std::filesystem::path downloads_dir();

    // Build a per-config cache directory name of the form
    // "idiff_cache_<json_stem>_<YYYYmmdd_HHMMSS>".  Characters unsafe
    // for filesystem paths in the stem are replaced with '_'.  The
    // timestamp uses local time and is fixed at call time.
    static std::string make_config_cache_dirname(
        const std::filesystem::path& json_file);

    // Return the local path where `url` is (or will be) cached.  The
    // path mirrors the URL structure: `<root>/<host>/<url_path>`.  This
    // is deterministic for a given URL and cache root; it is NOT a
    // guarantee that the file exists on disk.  Use fetch() to make
    // sure the file is present before reading.
    //
    // If register_urls() has been called with a non-empty set, the
    // common host and the longest common leading path segments shared
    // by every registered URL are stripped from the result so the
    // resulting tree is as shallow as possible (while still keeping
    // every registered URL at a unique path).  For URLs not in the
    // registered set -- or when register_urls() has not been called --
    // the full mirror structure is preserved.
    std::filesystem::path path_for(const std::string& url) const;

    // Teach the cache about the full set of URLs that belong to the
    // current session (typically the union of every comparison group's
    // URLs).  The cache uses this to compute the longest shared host /
    // path prefix and strip it from every emitted local path, making
    // the cache directory visually shallow.  Collisions are avoided:
    // if trimming a prefix segment would cause two URLs to map to the
    // same local path, that segment (and everything deeper) is kept.
    // Safe to call multiple times -- each call supersedes the previous
    // one.  Passing an empty vector resets the trimming to a no-op.
    void register_urls(const std::vector<std::string>& urls);

    // Make sure the given URL is present in the cache.  If the cached
    // file already exists (and is non-empty), this is a no-op and
    // returns the cached path.  Otherwise curl is invoked to download
    // it.  On failure an empty path is returned and `last_error()`
    // contains a human-readable reason.
    //
    // `force_refresh` bypasses the existence check and always re-runs
    // curl; useful when the remote asset is known to have changed.
    //
    // If a background prefetch for the same URL is already running,
    // this call waits for it to finish instead of kicking off a second
    // curl.  The net effect: prefetched files are returned instantly,
    // in-flight ones are transparently joined, and cold URLs are
    // downloaded on the foreground thread.
    std::filesystem::path fetch(const std::string& url,
                                 bool force_refresh = false);

    // Return true when the URL is already materialized on disk as a
    // non-empty file.  Cheap -- no network access.  Useful for UI hints
    // (e.g. "this group is ready, that one is still downloading").
    bool is_cached(const std::string& url) const;

    // Schedule `url` to be downloaded by the background worker pool.
    // Returns immediately.  Lower `priority` values are served first
    // (0 = most urgent).  Duplicate URLs already queued or in-flight
    // are silently ignored, so callers can re-issue the same set of
    // prefetches on every viewport change without extra bookkeeping.
    // A call to prefetch() lazily spins up the worker threads on first
    // use.
    void prefetch(const std::string& url, int priority = 10);

    // Drop every queued-but-not-yet-started prefetch task.  Tasks that
    // have already been picked up by a worker keep running -- cancelling
    // mid-curl would waste bandwidth we already spent.  Intended for use
    // when the "what to prefetch" plan changes (e.g. the user jumped to
    // a different comparison group and the old neighbours are no longer
    // interesting).
    void cancel_pending_prefetches();

    // Diagnostic from the most recent fetch() call.  Empty on success.
    const std::string& last_error() const noexcept { return last_error_; }

    // Directory that holds cached files.  Created lazily.
    const std::filesystem::path& root() const noexcept { return root_; }

private:
    std::filesystem::path root_;
    std::string last_error_;

    // Trimming state populated by register_urls().  When strip_host_ is
    // non-empty, path_for() drops that exact first-level host directory
    // from the output (the cache root is already per-config, so this
    // doesn't risk cross-config collisions).  strip_segments_ is the
    // number of leading path segments that are identical across every
    // registered URL and can therefore also be dropped.
    std::string strip_host_;
    std::size_t strip_segments_ = 0;

    // --- Background prefetch pool ------------------------------------
    //
    // Per-URL task state shared between fetch() (which may wait on the
    // result) and the worker threads (which fill it in).  We keep the
    // std::shared_ptr in the inflight_ map so even when a worker is
    // about to erase its map entry, a fetch() caller still holding a
    // copy of the shared_ptr can safely inspect the result.
    struct Task {
        std::mutex              m;
        std::condition_variable cv;
        std::string             url;
        std::filesystem::path   path;       // filled on success
        std::string             error;      // filled on failure
        bool                    done    = false;
        bool                    success = false;
        bool                    started = false;  // picked up by a worker
        int                     priority = 0;
        std::uint64_t           seq     = 0;      // FIFO tiebreaker
    };

    // Priority queue entry.  We intentionally store only the URL so
    // the Task shared_ptr stays reachable solely through inflight_ --
    // that makes "cancel pending" a simple queue-clear instead of
    // having to scrub dangling shared_ptr copies out of the queue.
    struct PendingRef {
        int           priority = 0;
        std::uint64_t seq      = 0;
        std::string   url;
        // Lower priority first; FIFO within same priority.  std::priority_queue
        // is a max-heap, so "less urgent" must compare greater.
        bool operator<(const PendingRef& o) const {
            if (priority != o.priority) return priority > o.priority;
            return seq > o.seq;
        }
    };

    mutable std::mutex              pool_mtx_;
    std::condition_variable         pool_cv_;
    std::priority_queue<PendingRef> queue_;
    std::unordered_map<std::string, std::shared_ptr<Task>> inflight_;
    std::vector<std::thread>        workers_;
    std::uint64_t                   next_seq_    = 0;
    bool                            stop_        = false;
    bool                            pool_started_ = false;

    // Lazily start the worker pool on first prefetch().  No-op once
    // the threads are already running.  Must be called with pool_mtx_
    // NOT held (it locks internally on first start).
    void ensure_pool_started();

    // Worker thread body.  Pulls the highest-priority pending URL,
    // marks its Task as started, releases the lock, runs curl, and
    // writes the result back into the Task under the Task's own mutex.
    void worker_loop();

    bool run_curl(const std::string& url,
                  const std::filesystem::path& dest,
                  std::string* out_error = nullptr);
};

} // namespace idiff

#endif // IDIFF_URL_CACHE_H
