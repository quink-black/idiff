#include "core/image_impl.h"

#include <limits>

#ifdef IDIFF_HAVE_FFMPEG
extern "C" {
#include <libavutil/frame.h>
}
#endif

namespace idiff {

#ifdef IDIFF_HAVE_FFMPEG
Image::Impl::~Impl() {
    if (src_av_frame) {
        av_frame_free(&src_av_frame);
    }
}
#endif

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

std::size_t Image::resident_bytes() const noexcept {
    if (!impl_ || impl_->mat.empty() || impl_->mat.rows <= 0) return 0;
    const std::size_t rows = static_cast<std::size_t>(impl_->mat.rows);
    if (impl_->mat.step[0] > std::numeric_limits<std::size_t>::max() / rows) {
        return std::numeric_limits<std::size_t>::max();
    }
    return impl_->mat.step[0] * rows;
}

const cv::Mat& Image::mat() const noexcept {
    static const cv::Mat empty{};
    if (!impl_) return empty;
    return impl_->mat;
}

int ImageInfo::display_width() const noexcept {
    if (sar_num > 0 && sar_den > 0 && (sar_num != 1 || sar_den != 1)) {
        // Round to nearest integer to avoid fractional display sizes.
        return (width * sar_num + sar_den / 2) / sar_den;
    }
    return width;
}

int ImageInfo::display_height() const noexcept {
    return height;
}

} // namespace idiff
