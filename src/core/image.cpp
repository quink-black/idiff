#include "core/image_impl.h"

namespace idiff {

Image::Image() : impl_(std::make_unique<Impl>()) {}

Image::~Image() = default;

Image::Image(Image&&) noexcept = default;
Image& Image::operator=(Image&&) noexcept = default;

const ImageInfo& Image::info() const noexcept {
    static const ImageInfo empty{};
    if (!impl_) return empty;
    return impl_->info;
}

const uint8_t* Image::pixels() const noexcept {
    if (!impl_ || impl_->mat.empty()) return nullptr;
    return impl_->mat.ptr<uint8_t>();
}

const cv::Mat& Image::mat() const noexcept {
    static const cv::Mat empty{};
    if (!impl_) return empty;
    return impl_->mat;
}

} // namespace idiff
