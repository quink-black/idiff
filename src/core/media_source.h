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

} // namespace idiff

#endif // IDIFF_MEDIA_SOURCE_H
