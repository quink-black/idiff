#ifndef IDIFF_METRICS_PANEL_H
#define IDIFF_METRICS_PANEL_H

#include "core/metrics_engine.h"

namespace idiff {

class Image;

class MetricsPanel {
public:
    MetricsPanel();
    ~MetricsPanel();

    void render(const Image* image_a, const Image* image_b);
    void render_inline(const Image* image_a, const Image* image_b);
    // Single-image statistics (RGB mean/variance).
    void render_single(const Image* image);

private:
    void draw_histogram(const Histogram& hist);

    bool compute_metrics_ = false;
    bool metrics_computed_ = false;
    double psnr_ = 0.0;
    double ssim_ = 0.0;
    double mse_ = 0.0;

    // Single-image metrics state
    bool single_computed_ = false;
    double mean_r_ = 0.0, mean_g_ = 0.0, mean_b_ = 0.0;
    double var_r_ = 0.0, var_g_ = 0.0, var_b_ = 0.0;

    // Histogram state (cached to avoid per-frame recomputation)
    bool hist_show_ = true;
    bool hist_cache_valid_ = false;
    Histogram cached_hist_{};
};

} // namespace idiff

#endif // IDIFF_METRICS_PANEL_H
