#include "core/image_comparator.h"
#include "core/image_impl.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

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

// Convert a cv::Mat to 8-bit 3-channel (RGB order) for comparison.
// Handles grayscale, RGBA, and 16-bit inputs.
cv::Mat normalize_for_comparison(const cv::Mat& mat) {
    cv::Mat result;

    // Convert 16-bit to 8-bit
    if (mat.depth() == CV_16U) {
        mat.convertTo(result, CV_8U, 1.0 / 257.0);
    } else {
        result = mat;
    }

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

} // namespace idiff
