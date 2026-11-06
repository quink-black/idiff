#ifndef IDIFF_GIF_MAGICK_SOURCE_H
#define IDIFF_GIF_MAGICK_SOURCE_H

#ifdef IDIFF_HAVE_MAGICK

#include "core/media_source.h"

#include <string>

namespace idiff {

// Multi-frame MediaSource for animated GIFs in builds compiled without
// FFmpeg.  Each read_frame() re-reads and re-coalesces the GIF, which
// honours disposal methods and frame transparency without retaining
// Q16-HDRI frames in memory between reads.
class GifMagickSource final : public MediaSource {
public:
    explicit GifMagickSource(std::string path);
    ~GifMagickSource() override;

    int frame_count() const noexcept override { return frame_count_; }
    int width() const noexcept override { return width_; }
    int height() const noexcept override { return height_; }
    const std::string& format_description() const noexcept override {
        return format_desc_;
    }
    std::unique_ptr<Image> read_frame(int index) override;
    const std::string& last_error() const noexcept override {
        return last_error_;
    }

    bool is_valid() const noexcept { return frame_count_ > 0; }

private:
    std::string path_;
    int frame_count_ = 0;
    int width_ = 0;
    int height_ = 0;
    std::string format_desc_;
    std::string last_error_;
};

} // namespace idiff

#endif // IDIFF_HAVE_MAGICK
#endif // IDIFF_GIF_MAGICK_SOURCE_H
