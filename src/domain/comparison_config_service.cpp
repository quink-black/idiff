#include "domain/comparison_config_service.h"

#include "util/logger.h"

#include <filesystem>
#include <unordered_map>
#include <utility>

namespace idiff {

namespace {

// Pull the human-friendly basename out of a URL.  Strips query and
// fragment, then keeps the segment after the last '/'.  Returns empty
// when nothing intelligible remains.
std::string url_basename(const std::string& url) {
    std::size_t end = url.size();
    if (auto q = url.find_first_of("?#"); q != std::string::npos) {
        end = q;
    }
    std::size_t slash = url.find_last_of('/', end ? end - 1 : 0);
    std::size_t beg = (slash == std::string::npos) ? 0 : slash + 1;
    if (beg >= end) return {};
    return url.substr(beg, end - beg);
}

} // namespace

ComparisonConfigService::ComparisonConfigService() = default;
ComparisonConfigService::~ComparisonConfigService() = default;

void ComparisonConfigService::set_cache_root_override(
        std::filesystem::path root) {
    cache_root_override_ = std::move(root);
}

LoadConfigResult ComparisonConfigService::load(const std::string& path) {
    LoadConfigResult result;

    ComparisonConfig cfg = load_comparison_config(path);
    if (!cfg.error.empty()) {
        result.ok = false;
        result.status_message = "Config: " + cfg.error;
        LOG_WARN("ComparisonConfigService::load: %s", cfg.error.c_str());
        return result;
    }

    // Resolve the cache directory before tearing down anything.  If
    // this fails the caller's previous session is preserved.
    std::filesystem::path cache_root = cache_root_override_.empty()
        ? UrlCache::resolve_default_root()
        : cache_root_override_;
    std::string cache_status;
    std::filesystem::path cache_dir = UrlCache::prepare_for_config(
        cache_root, std::filesystem::path(path), &cache_status);
    if (cache_dir.empty()) {
        result.ok = false;
        result.status_message = "Config load aborted: " + cache_status;
        LOG_WARN("ComparisonConfigService::load: cache prepare failed: %s",
                 cache_status.c_str());
        return result;
    }
    if (!cache_status.empty()) {
        LOG_INFO("ComparisonConfigService::load: %s at %s",
                 cache_status.c_str(), cache_dir.string().c_str());
    }

    config_ = std::move(cfg);
    current_index_ = -1;
    url_cache_ = std::make_unique<UrlCache>(cache_dir);

    // Register every URL up front so the cache can compute the
    // longest common prefix and keep the cache directory shallow.
    std::vector<std::string> all_urls;
    for (const auto& g : config_.groups) {
        for (const auto& it : g.items) {
            if (!it.url.empty()) all_urls.push_back(it.url);
        }
    }
    url_cache_->register_urls(all_urls);

    std::size_t total_items = 0;
    for (const auto& g : config_.groups) total_items += g.items.size();
    result.ok = true;
    result.status_message = "Loaded config: " +
        std::to_string(config_.groups.size()) + " group(s), " +
        std::to_string(total_items) + " image(s). " + cache_status;
    LOG_INFO("ComparisonConfigService::load: %zu group(s), %zu item(s)",
             config_.groups.size(), total_items);
    return result;
}

std::size_t ComparisonConfigService::group_count() const noexcept {
    return config_.groups.size();
}

SwitchGroupResult ComparisonConfigService::switch_to(int group_idx) {
    SwitchGroupResult result;

    if (group_idx < 0 ||
        group_idx >= static_cast<int>(config_.groups.size())) {
        result.ok = false;
        result.status_message = "Invalid group index";
        LOG_WARN("ComparisonConfigService::switch_to: out-of-range index %d",
                 group_idx);
        return result;
    }
    if (group_idx == current_index_) {
        // No-op: already showing this group.  Caller must not waste a
        // library teardown / load_images() round-trip on this case.
        result.ok = false;
        result.status_message = "Group already active";
        return result;
    }
    if (!url_cache_) {
        result.ok = false;
        result.status_message = "No cache configured for comparison group";
        LOG_WARN("ComparisonConfigService::switch_to: url_cache_ is null");
        return result;
    }

    const auto& group = config_.groups[group_idx];
    result.requested = group.items.size();

    // Cancel the previous prefetch plan -- the neighbours of the
    // newly-selected group are what the user is most likely to visit
    // next, not the neighbours of the previous one.  In-flight workers
    // are allowed to finish; their results land in the cache and stay
    // reusable.
    url_cache_->cancel_pending_prefetches();

    // Schedule prefetches for neighbours within +/- kPrefetchRadius
    // groups of the target.  Lower priority values are served first;
    // we use the absolute offset as the priority so groups closest to
    // the selection win the queue.
    constexpr int kPrefetchRadius = 3;
    const int total = static_cast<int>(config_.groups.size());
    for (int off = 1; off <= kPrefetchRadius; ++off) {
        for (int sign : {+1, -1}) {
            int idx = group_idx + sign * off;
            if (idx < 0 || idx >= total) continue;
            const auto& g = config_.groups[idx];
            for (const auto& it : g.items) {
                if (!it.url.empty()) {
                    url_cache_->prefetch(it.url, off);
                }
            }
        }
    }

    // Foreground fetch: blocks per URL (or joins an in-flight
    // background prefetch).  Failed URLs are dropped from the entry
    // list but counted in result.failures.
    std::vector<std::string> fetched_paths;
    fetched_paths.reserve(group.items.size());
    for (const auto& item : group.items) {
        auto p = url_cache_->fetch(item.url);
        if (p.empty()) {
            ++result.failures;
            result.last_error = url_cache_->last_error();
            continue;
        }
        fetched_paths.push_back(p.string());
    }

    // Build the (local-path -> ComparisonItem*) map then translate
    // into ComparisonGroupEntry records.  We use path_for() (not the
    // path returned by fetch()) so the lookup keys stay deterministic
    // even when fetch() falls back to a different path on retries.
    std::unordered_map<std::string, const ComparisonItem*> by_path;
    for (const auto& item : group.items) {
        auto p = url_cache_->path_for(item.url);
        by_path[p.string()] = &item;
    }

    result.entries.reserve(fetched_paths.size());
    for (const auto& local_path : fetched_paths) {
        ComparisonGroupEntry entry;
        entry.local_path = local_path;
        auto it = by_path.find(local_path);
        if (it != by_path.end() && it->second) {
            const auto& item = *it->second;
            entry.display_label = !item.title.empty()
                                  ? item.title : url_basename(item.url);
        }
        result.entries.push_back(std::move(entry));
    }

    current_index_ = group_idx;

    std::string msg = "Group \"" + group.name + "\": loaded " +
        std::to_string(result.entries.size()) + "/" +
        std::to_string(result.requested) + " image(s)";
    if (result.failures > 0) {
        msg += " (" + std::to_string(result.failures) + " download failure";
        if (result.failures > 1) msg += "s";
        msg += ": " + result.last_error + ")";
    }
    result.status_message = std::move(msg);
    result.ok = true;

    LOG_INFO("ComparisonConfigService::switch_to: group %d ok %zu/%zu",
             group_idx, result.entries.size(), result.requested);
    return result;
}

void ComparisonConfigService::clear() {
    config_ = ComparisonConfig{};
    current_index_ = -1;
    url_cache_.reset();
}

} // namespace idiff
