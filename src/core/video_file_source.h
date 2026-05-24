#ifndef IDIFF_VIDEO_FILE_SOURCE_H
#define IDIFF_VIDEO_FILE_SOURCE_H

#ifdef IDIFF_HAVE_FFMPEG

#include "core/media_source.h"
#include "core/video_decoder.h"

#include <memory>
#include <string>

namespace idiff {

// MediaSource backed by a video container file (MP4, MKV, MOV, AVI, etc.).
// Internally delegates to VideoDecoder for frame-accurate decoding with
// seek support.  The decoder instance is created once and reused for all
// frame requests.
class VideoFileSource final : public MediaSource {
public:
    explicit VideoFileSource(std::string path);
    ~VideoFileSource() override;

    // Non-copyable
    VideoFileSource(const VideoFileSource&) = delete;
    VideoFileSource& operator=(const VideoFileSource&) = delete;

    int frame_count() const noexcept override;
    int width() const noexcept override;
    int height() const noexcept override;
    const std::string& format_description() const noexcept override;
    std::unique_ptr<Image> read_frame(int index) override;
    std::unique_ptr<Image> read_keyframe(int index) override;
    const std::string& last_error() const noexcept override;

    const std::string& path() const noexcept { return path_; }

    // Whether the source was successfully opened.
    bool is_valid() const noexcept;

private:
    std::string path_;
    std::unique_ptr<VideoDecoder> decoder_;
    std::string format_desc_;
    std::string last_error_;
};

// Returns true if the file extension suggests a video container format
// that VideoFileSource can handle.
bool is_video_file_extension(const std::string& path) noexcept;

} // namespace idiff

#endif // IDIFF_HAVE_FFMPEG
#endif // IDIFF_VIDEO_FILE_SOURCE_H
