#ifndef IDIFF_METRICS_PANEL_H
#define IDIFF_METRICS_PANEL_H

#include "core/metrics_engine.h"

#include <unordered_map>
#include <string>
#include <utility>
#include <vector>

namespace idiff {

class Image;

struct PerImageStats {
    bool computed = false;
    SingleImageMetrics metrics{};
    bool hist_valid = false;
    Histogram histogram{};
};

class MetricsPanel {
public:
    MetricsPanel();
    ~MetricsPanel();

    void render_inline(const Image* image_a, const Image* image_b);

    // Multi-partner "A vs each" metrics.  Renders a table with one row per
    // partner (by display name) containing PSNR / SSIM / MSE computed
    // against the shared reference image_a.  Results are cached keyed by
    // (a, partner) pointer pairs so flipping tabs or idle re-renders do
    // not trigger recomputation; invalidation follows the same Image*
    // lifecycle as render_statistics.
    void render_pair_metrics(const Image* image_a,
                             const std::vector<std::pair<std::string,
                                                          const Image*>>& partners);

    // Multi-image statistics: each entry is (name, Image*).
    // Shows per-image stats with labels, lazy computation, and caching.
    // The cache is keyed by Image pointer, so it naturally invalidates when
    // an underlying image is replaced (e.g. upscale re-run, YUV frame step,
    // decoder-param change), and A/B slot swaps do not corrupt it.
    void render_statistics(const std::vector<std::pair<std::string, const Image*>>& images);

    // Drop all cached statistics.  Normally not needed because the cache is
    // keyed by Image* and is pruned every frame to only the images currently
    // shown; kept for callers that want to force-flush on project reload.
    void invalidate_cache();

private:
    void draw_histogram(const Histogram& hist, const char* title);
    void render_image_stats(const std::string& name, const Image* image,
                            PerImageStats& cache);

    bool compute_metrics_ = false;
    bool metrics_computed_ = false;
    double psnr_ = 0.0;
    double ssim_ = 0.0;
    double mse_ = 0.0;

    // Per-image statistics cache keyed by the Image pointer.  Using the
    // pointer (rather than the display label) means:
    //   * swapping A/B does not invalidate valid results;
    //   * when an image is replaced (new allocation) the old cache entry
    //     becomes unreachable from the input vector and is pruned in
    //     render_statistics;
    //   * two different entries that happen to share a display label do
    //     not collide.
    std::unordered_map<const Image*, PerImageStats> stats_cache_;
    // Cache for multi-partner A-vs-X metrics.  Keyed by the partner Image*
    // only: the shared reference A is invalidated implicitly by bumping
    // cached_ref_ below whenever A changes, which wipes the map.  That
    // keeps the key small and the invalidation rule obvious.
    struct PairMetrics {
        double psnr = 0.0;
        double ssim = 0.0;
        double mse = 0.0;
        bool computed = false;
    };
    std::unordered_map<const Image*, PairMetrics> pair_cache_;
    const Image* cached_ref_ = nullptr;
    // Hard cap to protect against pathological long sessions.  Any excess
    // entries beyond the images currently displayed are evicted.
    static constexpr std::size_t kMaxCacheEntries = 64;
    bool hist_show_ = true;
};

} // namespace idiff

#endif // IDIFF_METRICS_PANEL_H
