#ifndef IDIFF_MEDIA_SOURCE_H
#define IDIFF_MEDIA_SOURCE_H

#include <cstdint>
#include <memory>
#include <string>

#include "core/image.h"
#include "core/image_loader.h"

namespace idiff {

// MediaSource abstracts any input that produces one or more frames of
// pixel data that can be fed into the comparison viewport.  A still
// image is modeled as a MediaSource with frame_count() == 1; a raw
// video stream (e.g. a .yuv file) exposes its per-frame count.
//
// read_frame() is expected to return an Image in the same RGB/RGBA
// representation that the rest of the pipeline consumes (i.e. what
// ImageLoader produces today).  The Image is owned by the caller.
class MediaSource {
public:
    virtual ~MediaSource() = default;

    // Total number of frames the source exposes.  Must be >= 1.
    virtual int frame_count() const noexcept = 0;

    // Frame dimensions in pixels.  All frames of a single source share the
    // same dimensions.
    virtual int width() const noexcept = 0;
    virtual int height() const noexcept = 0;

    // Human-readable short description suitable for the Properties panel,
    // e.g. "PNG 8-bit RGB" or "YUV420P 1920x1080 8-bit".  May be empty.
    virtual const std::string& format_description() const noexcept = 0;

    // Decode and return frame `index`.  Out-of-range indices must return
    // nullptr; callers are expected to clamp beforehand.
    virtual std::unique_ptr<Image> read_frame(int index) = 0;

    // Last error message from the most recent read_frame() failure, if any.
    virtual const std::string& last_error() const noexcept = 0;
};

// Adapter that makes a single still-image file look like a 1-frame
// MediaSource.  Internally delegates to ImageLoader; the preferred
// backend can be changed between reads so users can toggle between
// ImageMagick / OpenCV decoders at runtime.
class ImageFileSource final : public MediaSource {
public:
    ImageFileSource(std::string path, LoaderBackend preferred_backend);
    ~ImageFileSource() override;

    int frame_count() const noexcept override { return 1; }
    int width() const noexcept override { return width_; }
    int height() const noexcept override { return height_; }
    const std::string& format_description() const noexcept override { return format_desc_; }
    std::unique_ptr<Image> read_frame(int index) override;
    const std::string& last_error() const noexcept override { return last_error_; }

    const std::string& path() const noexcept { return path_; }
    void set_preferred_backend(LoaderBackend backend) noexcept;

private:
    std::string path_;
    LoaderBackend preferred_backend_;
    int width_ = 0;
    int height_ = 0;
    std::string format_desc_;
    std::string last_error_;
};

// Pixel layouts supported for raw YUV streams.  All variants are planar
// and 8-bit per component.
enum class YuvPixelFormat {
    YUV420P,  // I420: Y plane followed by U plane (w/2 x h/2) then V plane
    YUV422P,  // Y plane followed by U (w/2 x h) then V (w/2 x h)
    YUV444P,  // Y, U, V all w x h
};

// Color range of the YUV samples.
enum class YuvColorRange {
    Limited,  // BT.601-style: Y in [16, 235], UV in [16, 240]
    Full,     // Y and UV in [0, 255]
};

// All parameters needed to decode a raw YUV file.  The file itself carries
// no metadata so the user (or a filename-based guess) must supply these.
struct YuvStreamParams {
    int width = 0;
    int height = 0;
    YuvPixelFormat pixel_format = YuvPixelFormat::YUV420P;
    YuvColorRange color_range = YuvColorRange::Limited;
};

// Bytes per frame for the given format, or 0 if params are invalid.
std::size_t yuv_frame_size_bytes(const YuvStreamParams& p) noexcept;

// Short human-readable label, e.g. "YUV420P".
const char* yuv_pixel_format_name(YuvPixelFormat f) noexcept;
const char* yuv_color_range_name(YuvColorRange r) noexcept;

// Heuristically guess YUV parameters from a file path.  Recognizes
// patterns like `name_1920x1080_yuv420p.yuv` or `name_nv12_1280x720.yuv`.
// Unknown fields are left at their existing value in `out`, so callers
// can prefill `out` with UI defaults and let the guess override what it
// can recognize.  Returns true if any field was populated.
bool guess_yuv_params_from_filename(const std::string& path,
                                    YuvStreamParams& out);

// MediaSource backed by a raw YUV file on disk.  Frames are read on
// demand using the configured parameters.  The file is opened lazily on
// the first successful read_frame().
class YuvRawSource final : public MediaSource {
public:
    YuvRawSource(std::string path, const YuvStreamParams& params);
    ~YuvRawSource() override;

    int frame_count() const noexcept override { return frame_count_; }
    int width() const noexcept override { return params_.width; }
    int height() const noexcept override { return params_.height; }
    const std::string& format_description() const noexcept override { return format_desc_; }
    std::unique_ptr<Image> read_frame(int index) override;
    const std::string& last_error() const noexcept override { return last_error_; }

    const std::string& path() const noexcept { return path_; }
    const YuvStreamParams& params() const noexcept { return params_; }

private:
    std::string path_;
    YuvStreamParams params_;
    std::size_t frame_bytes_ = 0;
    int frame_count_ = 0;
    std::string format_desc_;
    std::string last_error_;
};

} // namespace idiff

#endif // IDIFF_MEDIA_SOURCE_H
