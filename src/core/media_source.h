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

    // Fast approximate read for scrubbing: returns the nearest keyframe
    // at or before the requested index.  The returned frame may not
    // correspond to the exact index.  Default falls back to read_frame.
    virtual std::unique_ptr<Image> read_keyframe(int index) {
        return read_frame(index);
    }

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

    // Cheap openability probe: returns true when the file exists and
    // is readable.  Does NOT verify the format or decode any pixels --
    // a file that is the wrong format will still pass this check and
    // surface a decode error on the first read_frame() call.  Used by
    // the lazy-load open path so non-existent files are reported at
    // load time without forcing a full decode.
    bool is_valid() const noexcept;

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

// All parameters needed to decode a raw YUV file.  The file itself carries
// no metadata so the user (or a filename-based guess) must supply these.
//
// All format/metadata fields store FFmpeg names as strings.  This gives
// full coverage of every value FFmpeg supports without needing to mirror
// FFmpeg's enum constants:
//
//   pixel_format:  FFmpeg pixel format name, e.g. "yuv420p", "nv12",
//                  "p010le", "yuv444p12le".
//   color_range:   FFmpeg color range name: "tv" or "pc".
//   color_matrix:  FFmpeg color space name, e.g. "bt709", "bt2020-nccl".
//   color_primaries: FFmpeg color primaries name, e.g. "bt709", "bt2020".
//   transfer:      FFmpeg transfer characteristic name, e.g. "bt709",
//                  "smpte2084", "arib-std-b67".
struct YuvStreamParams {
    int width = 0;
    int height = 0;
    std::string pixel_format = "yuv420p";
    std::string color_range = "tv";
    std::string color_matrix = "bt709";
    std::string color_primaries = "bt709";
    std::string transfer = "bt709";
};

// Heuristically guess YUV resolution and pixel format from a file
// path.  Recognizes patterns like `name_1920x1080_yuv420p.yuv` or
// `name_nv12_1280x720.yuv`.  Only `width`, `height`, and
// `pixel_format` are touched; colour-metadata fields are deliberately
// left at their incoming values because filename hints for those tags
// are unreliable and silently picking the wrong colour space tends to
// produce frames that look plausible but are numerically wrong.
// Returns true if any field was populated.
bool guess_yuv_params_from_filename(const std::string& path,
                                    YuvStreamParams& out);

} // namespace idiff

#endif // IDIFF_MEDIA_SOURCE_H
