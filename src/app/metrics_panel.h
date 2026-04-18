#ifndef IDIFF_METRICS_PANEL_H
#define IDIFF_METRICS_PANEL_H

#include "core/metrics_engine.h"

#include <map>
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

    void render(const Image* image_a, const Image* image_b);
    void render_inline(const Image* image_a, const Image* image_b);

    // Multi-image statistics: each entry is (name, Image*).
    // Shows per-image stats with labels, lazy computation, and caching.
    void render_statistics(const std::vector<std::pair<std::string, const Image*>>& images);

    // Invalidate cached statistics for all images (call when selection changes
    // or images are reloaded).
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

    // Per-image statistics cache keyed by image name
    std::map<std::string, PerImageStats> stats_cache_;
    bool hist_show_ = true;
};

} // namespace idiff

#endif // IDIFF_METRICS_PANEL_H
