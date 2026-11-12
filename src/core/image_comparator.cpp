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

// Convert any supported input to a CV_32F scalar field on the
// 0..255 scale, without the 8-bit rounding step.  Reveal needs the
// sub-level precision: a watermark below one 8-bit step is invisible
// once the subtraction runs on quantized data.  Chroma fields are
// computed in float and never routed through an 8-bit conversion.
cv::Mat to_channel_float(const cv::Mat& mat, RevealChannel channel) {
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

    if (channel == RevealChannel::Luma) {
        cv::Mat gray;
        cv::cvtColor(f, gray, cv::COLOR_RGB2GRAY);
        return gray;
    }

    std::vector<cv::Mat> planes;
    cv::split(f, planes);  // R, G, B

    switch (channel) {
        case RevealChannel::Red:
            return planes[0];
        case RevealChannel::Green:
            return planes[1];
        case RevealChannel::Blue:
            return planes[2];
        case RevealChannel::ChromaRG: {
            cv::Mat out;
            cv::subtract(planes[0], planes[1], out);
            return out;
        }
        case RevealChannel::ChromaBY: {
            // B - (R + G) / 2
            cv::Mat rg_mean;
            cv::addWeighted(planes[0], 0.5, planes[1], 0.5, 0.0, rg_mean);
            cv::Mat out;
            cv::subtract(planes[2], rg_mean, out);
            return out;
        }
        case RevealChannel::LabA:
        case RevealChannel::LabB: {
            // cvtColor expects float RGB in [0, 1].  The 0..255 Lab
            // encoding used elsewhere in the app puts a* and b* around
            // 128, so shift them the same way.
            cv::Mat unit;
            f.convertTo(unit, CV_32F, 1.0 / 255.0);
            cv::Mat lab;
            cv::cvtColor(unit, lab, cv::COLOR_RGB2Lab);
            std::vector<cv::Mat> lab_planes;
            cv::split(lab, lab_planes);
            const int index = channel == RevealChannel::LabA ? 1 : 2;
            cv::Mat out = lab_planes[index] + 128.0;
            return out;
        }
        case RevealChannel::Luma:
            break;
    }
    return planes[1];
}

// Gaussian blur that evaluates large sigmas on a reduced copy: a 4K
// frame with sigma 100 would otherwise need a ~800-tap kernel.  The
// result is smooth by construction, so blurring at 1/factor resolution
// with sigma/factor and scaling back is indistinguishable from the
// full-resolution result, and keeps the slider interactive.
cv::Mat blur_reduced(const cv::Mat& src, double sigma) {
    const double s = std::max(0.1, sigma);
    constexpr double kMaxDirectSigma = 4.0;
    int factor = std::max(
        1, static_cast<int>(std::ceil(s / kMaxDirectSigma)));
    // Never scale a side below two pixels: a tiny image with a large
    // radius would otherwise ask cv::resize for a zero-sized output
    // and throw out of the render loop.
    factor = std::min(
        factor, std::max(1, std::min(src.cols, src.rows) / 2));

    if (factor <= 1) {
        cv::Mat out;
        cv::GaussianBlur(src, out, cv::Size(0, 0), s);
        return out;
    }

    const cv::Size small_size(
        std::max(1, src.cols / factor),
        std::max(1, src.rows / factor));
    cv::Mat small;
    cv::resize(src, small, small_size, 0.0, 0.0, cv::INTER_AREA);
    cv::GaussianBlur(small, small, cv::Size(0, 0), s / factor);
    cv::Mat out;
    cv::resize(small, out, src.size(), 0.0, 0.0, cv::INTER_LINEAR);
    return out;
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

    // Work on the requested field.  Subtracting in float keeps
    // sub-8-bit structure from 16-bit sources; a chroma field must never
    // pass through an 8-bit conversion or its low bits are lost.
    cv::Mat field = to_channel_float(mat, options.channel);
    // One NaN would spread across its whole blur footprint.
    cv::patchNaNs(field, 0.0);

    // A sub-pixel blur removes sensor / compression grain without
    // touching structures wider than about a pixel.
    constexpr double kSmoothSigma = 0.8;
    cv::Mat base = field;
    if (options.smooth) {
        cv::GaussianBlur(base, base, cv::Size(0, 0), kSmoothSigma);
    }

    const double sigma = std::max(0.1, options.background_sigma);
    cv::Mat background = blur_reduced(base, sigma);

    cv::Mat residual;
    cv::subtract(base, background, residual, cv::noArray(), CV_32F);
    if (options.smooth) {
        cv::GaussianBlur(residual, residual, cv::Size(0, 0), kSmoothSigma);
    }

    // Optional local normalisation: divide by a blurred magnitude so each
    // region is encoded against its own residual amplitude.  The divisor
    // is floored to keep genuinely flat areas from amplifying
    // quantisation noise into grain.
    double encode_gain = options.amplification;
    if (options.normalize) {
        cv::Mat magnitude = cv::abs(residual);
        magnitude = blur_reduced(
            magnitude, std::max(1.0, options.normalize_sigma));
        cv::max(magnitude, std::max(1e-3, options.normalize_floor),
                magnitude);
        cv::divide(residual, magnitude, residual);
        // The normalised residual is dimensionless and mostly within a
        // few units, so the previous "levels per gain" scale would
        // saturate almost everything.  Keep the slider monotonic in
        // brightness while mapping it onto that smaller range.
        constexpr double kNormalisedGain = 0.45;
        encode_gain *= kNormalisedGain;
    }

    // Signed residual -> 0..255 with mid-gray as "no difference".
    // Saturating cast clamps the rare strong edge pixels.
    cv::Mat encoded;
    residual.convertTo(encoded, CV_8U, encode_gain, 128.0);

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
