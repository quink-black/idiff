// Internal header: defines Image::Impl for use by core library .cpp files.
// Do NOT include this from public headers.

#ifndef IDIFF_IMAGE_IMPL_H
#define IDIFF_IMAGE_IMPL_H

#include "core/image.h"

#include <opencv2/core.hpp>

#ifdef IDIFF_HAVE_FFMPEG
// Forward-declare AVFrame so this header does not pull libavutil into
// every translation unit that already includes image_impl.h.
struct AVFrame;
#endif

namespace idiff {

struct Image::Impl {
    cv::Mat mat;
    ImageInfo info;

#ifdef IDIFF_HAVE_FFMPEG
    // Optional reference to the decoder's source-domain AVFrame for
    // this image.  Populated by VideoFileSource::read_frame() via
    // av_frame_clone(), which only bumps refcounts on the underlying
    // pixel buffers -- no deep copy happens here.  Stays nullptr for
    // still-image sources (PNG / JPEG / etc.).
    //
    // The pixel sampler / inspector reads ground-truth values from
    // this frame (native pix_fmt, native bit depth, original color
    // tags) so high-bit-depth and HDR sources are not silently
    // truncated to the 8-bit RGB24 stored in `mat`.
    //
    // Lifetime: owned by Impl.  ~Impl() calls av_frame_free on it.
    // Move semantics on Image transfer ownership; copies are deleted
    // on the public Image type so we never end up with two Impls
    // owning the same ref.
    AVFrame* src_av_frame = nullptr;

    Impl() = default;
    ~Impl();
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;
#endif
};

} // namespace idiff

#endif // IDIFF_IMAGE_IMPL_H
