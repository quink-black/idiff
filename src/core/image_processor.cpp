#include "core/image_processor.h"
#include "core/image_impl.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

namespace idiff {

namespace {

int upscale_method_to_cv(UpscaleMethod method) {
    switch (method) {
        case UpscaleMethod::Nearest:  return cv::INTER_NEAREST;
        case UpscaleMethod::Bilinear: return cv::INTER_LINEAR;
        case UpscaleMethod::Bicubic:  return cv::INTER_CUBIC;
        case UpscaleMethod::Lanczos:  return cv::INTER_LANCZOS4;
    }
    return cv::INTER_LANCZOS4;
}

} // namespace

std::unique_ptr<Image> ImageProcessor::upscale(const Image& src, const UpscaleOptions& options) {
    last_error_.clear();

    if (options.target_width <= 0 || options.target_height <= 0) {
        last_error_ = "Invalid target dimensions";
        return nullptr;
    }

    const auto& mat = src.mat();
    if (mat.empty()) {
        last_error_ = "Source image is empty";
        return nullptr;
    }

    cv::Mat resized;
    cv::resize(mat, resized, cv::Size(options.target_width, options.target_height),
               0, 0, upscale_method_to_cv(options.method));

    auto result = std::make_unique<Image>();
    result->internal().mat = std::move(resized);
    result->internal().info = src.info();
    result->internal().info.width = options.target_width;
    result->internal().info.height = options.target_height;

    return result;
}

std::unique_ptr<Image> ImageProcessor::upscale_to_match(const Image& src, const Image& reference) {
    UpscaleOptions options;
    options.method = UpscaleMethod::Lanczos;
    options.target_width = reference.info().width;
    options.target_height = reference.info().height;

    if (src.info().width >= reference.info().width &&
        src.info().height >= reference.info().height) {
        last_error_ = "Source image is not smaller than reference";
        return nullptr;
    }

    return upscale(src, options);
}

const std::string& ImageProcessor::last_error() const noexcept {
    return last_error_;
}

} // namespace idiff
