#ifndef IDIFF_METRICS_ENGINE_H
#define IDIFF_METRICS_ENGINE_H

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include "core/image.h"

namespace idiff {

struct MetricsResult {
    double psnr = 0.0;
    double ssim = 0.0;
    double mse = 0.0;
};

struct SingleImageMetrics {
    double mean_r = 0.0;
    double mean_g = 0.0;
    double mean_b = 0.0;
    double var_r = 0.0;
    double var_g = 0.0;
    double var_b = 0.0;
    double min_r = 0.0;
    double min_g = 0.0;
    double min_b = 0.0;
    double max_r = 0.0;
    double max_g = 0.0;
    double max_b = 0.0;
};

struct Histogram {
    std::array<uint32_t, 256> r{};
    std::array<uint32_t, 256> g{};
    std::array<uint32_t, 256> b{};
};

class MetricsEngine {
public:
    // Both images must have identical dimensions and pixel format.
    std::optional<MetricsResult> compute(const Image& a, const Image& b);

    std::optional<double> compute_psnr(const Image& a, const Image& b);
    std::optional<double> compute_ssim(const Image& a, const Image& b);
    std::optional<double> compute_mse(const Image& a, const Image& b);
    std::optional<SingleImageMetrics> compute_single(const Image& img);
    std::optional<Histogram> compute_histogram(const Image& img);

    const std::string& last_error() const noexcept;

private:
    std::string last_error_;
};

} // namespace idiff

#endif // IDIFF_METRICS_ENGINE_H
