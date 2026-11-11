#ifdef IDIFF_HAVE_MAGICK

#include "core/gif_magick_source.h"

#include "core/image.h"
#include "core/image_loader.h"

#include <utility>

namespace idiff {

GifMagickSource::GifMagickSource(std::string path)
    : path_(std::move(path)) {
    ImageLoader loader;
    frame_count_ = loader.gif_frame_count(path_);
    if (frame_count_ <= 0) {
        last_error_ = loader.last_error();
        return;
    }
    frame_rate_ = loader.gif_frame_rate(path_);

    // Decode frame 0 once to learn the canvas dimensions.  The pixels
    // are discarded; the timeline decodes lazily on demand.
    auto first = loader.load_gif_frame(path_, 0);
    if (!first) {
        last_error_ = loader.last_error();
        frame_count_ = 0;
        return;
    }
    const auto& info = first->info();
    width_ = info.width;
    height_ = info.height;
    format_desc_ = "GIF " + std::to_string(width_) + "x"
                 + std::to_string(height_) + " "
                 + std::to_string(frame_count_) + " frames";
}

GifMagickSource::~GifMagickSource() = default;

std::unique_ptr<Image> GifMagickSource::read_frame(int index) {
    if (index < 0 || index >= frame_count_) {
        last_error_ = "frame index out of range";
        return nullptr;
    }

    ImageLoader loader;
    auto img = loader.load_gif_frame(path_, index);
    if (!img) {
        last_error_ = loader.last_error();
        return nullptr;
    }
    last_error_.clear();
    return img;
}

} // namespace idiff

#endif // IDIFF_HAVE_MAGICK
