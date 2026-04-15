// Internal header: defines Image::Impl for use by core library .cpp files.
// Do NOT include this from public headers.

#ifndef IDIFF_IMAGE_IMPL_H
#define IDIFF_IMAGE_IMPL_H

#include "core/image.h"

#include <opencv2/core.hpp>

namespace idiff {

struct Image::Impl {
    cv::Mat mat;
    ImageInfo info;
};

} // namespace idiff

#endif // IDIFF_IMAGE_IMPL_H
