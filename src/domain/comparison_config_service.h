#ifndef IDIFF_DOMAIN_COMPARISON_CONFIG_SERVICE_H
#define IDIFF_DOMAIN_COMPARISON_CONFIG_SERVICE_H

// Comparison-configuration service.
//
// Owns the parsed ComparisonConfig (groups + items) and the per-config
// UrlCache.  Exposes two commands the App orchestrates:
//
//   load(path):       parse JSON, prepare cache directory, register
//                     URLs.  Does not load any pixels yet.
//   switch_to(idx):   fetch the target group's URLs to disk, schedule
//                     prefetches for nearby groups, and return the
//                     local paths plus per-path display labels so the
//                     caller can hand them to load_images() and then
//                     re-label entries.
//
// The service deliberately does NOT touch the image library, selection,
// or diff cache.  Tearing those down between groups is the App's
// responsibility -- the service only reports what to load and what to
// label, never mutating any other domain state.  This split keeps the
// service usable in tests where there is no library.
//
// Threading: the service is single-threaded from the caller's side.
// The owned UrlCache spawns its own background prefetch worker pool,
// fully encapsulated; switch_to()'s foreground fetch() may block on
// curl invocations.

#include "core/comparison_config.h"
#include "core/url_cache.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace idiff {

// Returned by load().  On failure `ok == false` and `status_message`
// carries a human-readable reason; the previous config (if any) is
// left intact in the service.
struct LoadConfigResult {
    bool ok = false;
    std::string status_message;
};

// One entry of switch_to()'s output: a file the caller should hand to
// load_images() plus the human-friendly label that should override the
// auto-derived filename after load.  `display_label` is empty when the
// config supplied neither a title nor a derivable URL basename, in
// which case the caller should keep load_images()'s default labelling.
struct ComparisonGroupEntry {
    std::string local_path;
    std::string display_label;
};

// Returned by switch_to().  `entries` is the flat list of fetched
// images to load (in group order, failures dropped).  `failures` is
// the count of URLs that could not be downloaded; `last_error` carries
// the last UrlCache::last_error() string for those failures.
// `status_message` is a ready-to-display summary.  When `ok == false`
// the caller may still inspect `status_message` for the reason; the
// current group index is left untouched.
struct SwitchGroupResult {
    bool ok = false;
    std::vector<ComparisonGroupEntry> entries;
    std::size_t requested = 0;   // items in the target group
    std::size_t failures = 0;
    std::string last_error;
    std::string status_message;
};

class ComparisonConfigService {
public:
    ComparisonConfigService();
    ~ComparisonConfigService();

    ComparisonConfigService(const ComparisonConfigService&) = delete;
    ComparisonConfigService& operator=(const ComparisonConfigService&) = delete;

    // Override the cache-root directory used by load().  When empty,
    // load() defaults to the JSON file's own directory.  Tests use
    // this so they never write into the user's real Downloads folder.
    // The override only takes effect for subsequent load() calls.
    void set_cache_root_override(std::filesystem::path root);

    // Parse the JSON file at `path`, prepare the per-config cache
    // directory, and register every URL up front so the cache can
    // compute its shared-prefix trim.  The previous config (and its
    // cache) is dropped on success only.  After a successful load the
    // caller normally calls switch_to(0) to bring in the first group.
    LoadConfigResult load(const std::string& path);

    // Number of groups in the loaded config (0 when no config is
    // active).
    std::size_t group_count() const noexcept;

    // True iff load() has completed successfully.
    bool has_config() const noexcept { return !config_.groups.empty(); }

    // Index of the group whose URLs were most recently fetched, or
    // -1 when no group has been activated since the last load().
    int current_index() const noexcept { return current_index_; }

    // Read-only access to the parsed config for UI listing (group
    // names, descriptions, item counts).  Stable until the next load().
    const ComparisonConfig& config() const noexcept { return config_; }

    // Fetch every URL of `group_idx` to disk (reusing cached entries),
    // schedule background prefetches for neighbouring groups within a
    // small radius, and return the local paths plus display labels for
    // load_images() / re-labelling.  Out-of-range or no-op-when-already-
    // current calls return ok=false with `entries` empty; the current
    // index is updated only when the group changes.
    SwitchGroupResult switch_to(int group_idx);

    // Drop the loaded config and tear down the URL cache (cancelling
    // background prefetches).  Safe to call when nothing is loaded.
    void clear();

    // Test-only: borrow the underlying UrlCache so a fixture can call
    // path_for() to know where to stage files before switch_to() runs
    // (avoiding the curl invocation entirely).  Returns nullptr when
    // no config has been loaded yet.  The pointer is invalidated by
    // load() / clear().
    UrlCache* url_cache_for_test() const noexcept { return url_cache_.get(); }

private:
    ComparisonConfig config_;
    int current_index_ = -1;
    std::unique_ptr<UrlCache> url_cache_;
    std::filesystem::path cache_root_override_;
};

} // namespace idiff

#endif // IDIFF_DOMAIN_COMPARISON_CONFIG_SERVICE_H
