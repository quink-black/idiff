#ifndef IDIFF_METRICS_PANEL_H
#define IDIFF_METRICS_PANEL_H

namespace idiff {

class Image;

class MetricsPanel {
public:
    MetricsPanel();
    ~MetricsPanel();

    void render(const Image* image_a, const Image* image_b);
    void render_inline(const Image* image_a, const Image* image_b);

private:
    bool compute_metrics_ = false;
    bool metrics_computed_ = false;
    double psnr_ = 0.0;
    double ssim_ = 0.0;
    double mse_ = 0.0;
};

} // namespace idiff

#endif // IDIFF_METRICS_PANEL_H
