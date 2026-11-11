#include "core/image_comparator.h"
#include "core/depth_utils.h"
#include "core/image_impl.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>

namespace idiff {

namespace {

cv::Mat apply_colormap(const cv::Mat& gray, HeatmapColor color) {
    cv::Mat colored;
    int cmap = cv::COLORMAP_INFERNO;

    switch (color) {
        case HeatmapColor::Gray:     return gray.clone();
        case HeatmapColor::Inferno:  cmap = cv::COLORMAP_INFERNO; break;
        case HeatmapColor::Viridis:  cmap = cv::COLORMAP_VIRIDIS; break;
        case HeatmapColor::Coolwarm: cmap = cv::COLORMAP_COOL; break;
    }

    cv::Mat normalized;
    cv::normalize(gray, normalized, 0, 255, cv::NORM_MINMAX, CV_8U);
    cv::applyColorMap(normalized, colored, cmap);
    // applyColorMap outputs BGR; convert to RGB for consistency
    cv::cvtColor(colored, colored, cv::COLOR_BGR2RGB);
    return colored;
}

// Map an already-quantized 8-bit field through a palette without
// rescaling it.  The reveal filter relies on mid-gray meaning "no
// residual", which NORM_MINMAX would destroy.
cv::Mat apply_colormap_raw(const cv::Mat& gray, HeatmapColor color) {
    if (color == HeatmapColor::Gray) return gray.clone();

    int cmap = cv::COLORMAP_INFERNO;
    switch (color) {
        case HeatmapColor::Gray:     break;  // handled above
        case HeatmapColor::Inferno:  cmap = cv::COLORMAP_INFERNO; break;
        case HeatmapColor::Viridis:  cmap = cv::COLORMAP_VIRIDIS; break;
        case HeatmapColor::Coolwarm: cmap = cv::COLORMAP_COOL; break;
    }

    cv::Mat colored;
    cv::applyColorMap(gray, colored, cmap);
    cv::cvtColor(colored, colored, cv::COLOR_BGR2RGB);
    return colored;
}

// Convert any supported input to a CV_32F luminance field on the
// 0..255 scale, without the 8-bit rounding step.  Reveal needs the
// sub-level precision: a watermark below one 8-bit step is invisible
// once the subtraction runs on quantized data.
cv::Mat to_luminance_float(const cv::Mat& mat) {
    cv::Mat f;
    switch (mat.depth()) {
        case CV_16U:
            mat.convertTo(f, CV_32F, 1.0 / 257.0);
            break;
        case CV_16S:
            // Mirror convert_to_8u: shift the signed range first.
            mat.convertTo(f, CV_32F, 1.0 / 257.0, 32768.0 / 257.0);
            break;
        case CV_32F: {
            // Mirror convert_to_8u: float input is in [0, 1].
            cv::Mat clamped;
            cv::min(mat, 1.0, clamped);
            cv::max(clamped, 0.0, clamped);
            clamped.convertTo(f, CV_32F, 255.0);
            break;
        }
        default:
            mat.convertTo(f, CV_32F);
            break;
    }

    if (f.channels() == 4) {
        cv::cvtColor(f, f, cv::COLOR_RGBA2RGB);
    } else if (f.channels() == 1) {
        cv::cvtColor(f, f, cv::COLOR_GRAY2RGB);
    }

    cv::Mat gray;
    cv::cvtColor(f, gray, cv::COLOR_RGB2GRAY);
    return gray;
}

// Convert a cv::Mat to 8-bit 3-channel (RGB order) for comparison.
// Handles grayscale, RGBA, and high-depth inputs.
cv::Mat normalize_for_comparison(const cv::Mat& mat) {
    cv::Mat result = convert_to_8u(mat);

    // Convert to 3-channel
    if (result.channels() == 1) {
        cv::cvtColor(result, result, cv::COLOR_GRAY2RGB);
    } else if (result.channels() == 4) {
        cv::cvtColor(result, result, cv::COLOR_RGBA2RGB);
    }

    return result;
}

} // namespace

std::unique_ptr<Image> ImageComparator::compute_difference(const Image& a, const Image& b,
                                                            const DifferenceOptions& options) {
    last_error_.clear();

    const auto& info_a = a.info();
    const auto& info_b = b.info();

    if (info_a.width != info_b.width || info_a.height != info_b.height) {
        last_error_ = "Images must have identical dimensions for comparison";
        return nullptr;
    }

    // Normalize both images to 8-bit RGB for robust comparison
    cv::Mat mat_a = normalize_for_comparison(a.mat());
    cv::Mat mat_b = normalize_for_comparison(b.mat());

    cv::Mat diff;
    cv::absdiff(mat_a, mat_b, diff);

    if (options.amplification != 1.0) {
        diff.convertTo(diff, -1, options.amplification);
    }

    if (options.threshold > 0) {
        cv::threshold(diff, diff, options.threshold, 255, cv::THRESH_TOZERO);
    }

    auto result = std::make_unique<Image>();
    result->internal().mat = std::move(diff);
    result->internal().info.width = info_a.width;
    result->internal().info.height = info_a.height;
    result->internal().info.pixel_format = PixelFormat::RGB8;
    result->internal().info.bit_depth = 8;
    result->internal().info.has_alpha = false;
    result->internal().info.color_space = "Difference";

    return result;
}

std::unique_ptr<Image> ImageComparator::compute_heatmap(const Image& diff,
                                                         const DifferenceOptions& options) {
    last_error_.clear();

    const auto& mat = diff.mat();
    if (mat.empty()) {
        last_error_ = "Empty difference image";
        return nullptr;
    }

    cv::Mat gray;
    if (mat.channels() > 1) {
        // Diff image is in RGB order
        cv::cvtColor(mat, gray, cv::COLOR_RGB2GRAY);
    } else {
        gray = mat;
    }

    // apply_colormap returns RGB
    cv::Mat colored = apply_colormap(gray, options.heatmap_color);

    auto result = std::make_unique<Image>();
    result->internal().mat = std::move(colored);
    result->internal().info = diff.info();
    result->internal().info.pixel_format = PixelFormat::RGB8;
    result->internal().info.bit_depth = 8;
    result->internal().info.has_alpha = false;
    result->internal().info.color_space = "Heatmap";

    return result;
}

const std::string& ImageComparator::last_error() const noexcept {
    return last_error_;
}

std::unique_ptr<Image> ImageComparator::compute_reveal(const Image& src,
                                                        const RevealOptions& options) {
    last_error_.clear();

    const cv::Mat& mat = src.mat();
    if (mat.empty()) {
        last_error_ = "Empty image";
        return nullptr;
    }

    // Work on luminance: watermarks are low-amplitude luminance
    // offsets, and a single channel avoids amplifying per-channel
    // compression noise independently.  Subtracting in float keeps
    // sub-8-bit structure from 16-bit sources.
    cv::Mat gray = to_luminance_float(mat);

    // A sub-pixel blur removes sensor / compression grain without
    // touching structures wider than about a pixel.
    constexpr double kSmoothSigma = 0.8;
    cv::Mat base = gray;
    if (options.smooth) {
        cv::GaussianBlur(base, base, cv::Size(0, 0), kSmoothSigma);
    }

    const double sigma = std::max(0.1, options.background_sigma);
    cv::Mat background;
    // Large sigmas are evaluated on a reduced copy: a 4K frame with
    // sigma 100 would otherwise need a ~800-tap kernel.  The
    // background field is smooth by construction, so blurring at 1/f
    // resolution with sigma/f and scaling back is indistinguishable
    // from the full-resolution result, and keeps the slider
    // interactive.
    constexpr double kMaxDirectSigma = 4.0;
    int factor = std::max(
        1, static_cast<int>(std::ceil(sigma / kMaxDirectSigma)));
    // Never scale a side below two pixels: a tiny image with a large
    // radius would otherwise ask cv::resize for a zero-sized output
    // and throw out of the render loop.
    factor = std::min(
        factor, std::max(1, std::min(base.cols, base.rows) / 2));
    if (factor > 1) {
        const cv::Size small_size(
            std::max(1, base.cols / factor),
            std::max(1, base.rows / factor));
        cv::Mat small;
        cv::resize(base, small, small_size, 0.0, 0.0, cv::INTER_AREA);
        cv::GaussianBlur(small, small, cv::Size(0, 0), sigma / factor);
        cv::resize(small, background, base.size(), 0.0, 0.0,
                   cv::INTER_LINEAR);
    } else {
        cv::GaussianBlur(base, background, cv::Size(0, 0), sigma);
    }

    cv::Mat residual;
    cv::subtract(base, background, residual, cv::noArray(), CV_32F);
    if (options.smooth) {
        cv::GaussianBlur(residual, residual, cv::Size(0, 0), kSmoothSigma);
    }

    // Signed residual -> 0..255 with mid-gray as "no difference".
    // Saturating cast clamps the rare strong edge pixels.
    cv::Mat encoded;
    residual.convertTo(encoded, CV_8U, options.amplification, 128.0);

    cv::Mat colored = apply_colormap_raw(encoded, options.heatmap_color);

    auto result = std::make_unique<Image>();
    result->internal().mat = std::move(colored);
    result->internal().info = src.info();
    result->internal().info.pixel_format = PixelFormat::RGB8;
    result->internal().info.bit_depth = 8;
    result->internal().info.has_alpha = false;
    result->internal().info.color_space = "Reveal";

    return result;
}

} // namespace idiff
