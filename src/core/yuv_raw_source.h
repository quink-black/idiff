#ifndef IDIFF_YUV_RAW_SOURCE_H
#define IDIFF_YUV_RAW_SOURCE_H

#ifdef IDIFF_HAVE_FFMPEG

#include <cstdint>
#include <memory>
#include <string>

#include "core/image.h"
#include "core/media_source.h"

extern "C" {
#include <libavutil/pixfmt.h>
#include <libavutil/frame.h>
}

namespace idiff {

// Map YuvPixelFormat to the corresponding AVPixelFormat used by
// FFmpeg's rawvideo demuxer.
AVPixelFormat yuv_pixel_format_to_av(YuvPixelFormat f) noexcept;

// Map color enums to FFmpeg AVColor* constants.
AVColorSpace yuv_color_matrix_to_av(YuvColorMatrix m) noexcept;
AVColorPrimaries yuv_color_primaries_to_av(YuvColorPrimaries p) noexcept;

// MediaSource backed by a raw YUV file on disk, decoded via FFmpeg's
// rawvideo demuxer and converted to sRGB through VideoFilterGraph.
// Frames are read on demand using the configured parameters.  The
// FFmpeg context is opened lazily on the first read_frame() and reused
// for subsequent reads.
class YuvRawSource final : public MediaSource {
public:
    YuvRawSource(std::string path, const YuvStreamParams& params);
    ~YuvRawSource() override;

    // Non-copyable (owns FFmpeg contexts)
    YuvRawSource(const YuvRawSource&) = delete;
    YuvRawSource& operator=(const YuvRawSource&) = delete;

    int frame_count() const noexcept override { return frame_count_; }
    int width() const noexcept override { return params_.width; }
    int height() const noexcept override { return params_.height; }
    const std::string& format_description() const noexcept override { return format_desc_; }
    std::unique_ptr<Image> read_frame(int index) override;
    const std::string& last_error() const noexcept override { return last_error_; }

    const std::string& path() const noexcept { return path_; }
    const YuvStreamParams& params() const noexcept { return params_; }

private:
    struct FfCtx;
    bool open_ffmpeg();
    void close_ffmpeg() noexcept;
    bool decode_next_frame();

    std::string path_;
    YuvStreamParams params_;
    std::size_t frame_bytes_ = 0;
    int frame_count_ = 0;
    std::string format_desc_;
    std::string last_error_;

    std::unique_ptr<FfCtx> ff_;
    int ff_current_idx_ = -1;
};

} // namespace idiff

#endif // IDIFF_HAVE_FFMPEG
#endif // IDIFF_YUV_RAW_SOURCE_H
