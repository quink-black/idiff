#ifndef IDIFF_IMAGE_PROCESSOR_H
#define IDIFF_IMAGE_PROCESSOR_H

#include <memory>
#include <string>

#include "core/image.h"

namespace idiff {

enum class UpscaleMethod {
    Nearest,
    Bilinear,
    Bicubic,
    Lanczos,
};

struct UpscaleOptions {
    UpscaleMethod method = UpscaleMethod::Lanczos;
    int target_width = 0;
    int target_height = 0;
};

class ImageProcessor {
public:
    // Upscale image to target dimensions using the specified method.
    // Returns a new Image; the original is unmodified.
    std::unique_ptr<Image> upscale(const Image& src, const UpscaleOptions& options);

    // Convenience: upscale to match another image's dimensions.
    std::unique_ptr<Image> upscale_to_match(const Image& src, const Image& reference);

    const std::string& last_error() const noexcept;

private:
    std::string last_error_;
};

} // namespace idiff

#endif // IDIFF_IMAGE_PROCESSOR_H
