#ifndef IDIFF_VIDEO_DECODER_H
#define IDIFF_VIDEO_DECODER_H

#ifdef IDIFF_HAVE_FFMPEG

#include <cstdint>
#include <memory>
#include <string>

#include <opencv2/core.hpp>

namespace idiff {

// Rotation mode for video frames.  Used both for auto-detected rotation
// from container metadata and for manual user override.
enum class VideoRotation {
    None = 0,      // No rotation
    CW90 = 90,     // 90° clockwise
    CW180 = 180,   // 180°
    CW270 = 270,   // 270° clockwise (= 90° counter-clockwise)
};

// FFmpeg-based video decoder.  Opens a container file (MP4, MKV, MOV, etc.),
// finds the first video stream, and decodes frames on demand.  The decoder
// instance is reused across all frame requests (no per-frame allocation).
//
// Supports:
//   - Sequential decoding (next frame)
//   - Random-access seek (av_seek_frame to nearest keyframe, then decode forward)
//   - Current-frame caching (repeated read_frame(N) returns cached result)
//   - Auto-rotation from container display matrix metadata
//   - Manual rotation override
//
// Thread safety: NOT thread-safe.  All calls must be serialized by the caller.
class VideoDecoder {
public:
    VideoDecoder();
    ~VideoDecoder();

    // Non-copyable, non-movable (owns FFmpeg contexts)
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;
    VideoDecoder(VideoDecoder&&) = delete;
    VideoDecoder& operator=(VideoDecoder&&) = delete;

    // Open a video file.  Returns true on success.
    // On failure, last_error() contains the reason.
    bool open(const std::string& path);

    // Close and release all FFmpeg resources.
    void close();

    // Whether a file is currently open and ready for decoding.
    bool is_open() const noexcept;

    // --- Metadata (valid after successful open()) ---

    // Dimensions of the decoded frame AFTER rotation is applied.
    // For 90°/270° rotation, width and height are swapped relative to
    // the coded dimensions.
    int width() const noexcept;
    int height() const noexcept;

    // Coded (pre-rotation) dimensions.
    int coded_width() const noexcept;
    int coded_height() const noexcept;

    int frame_count() const noexcept;
    double fps() const noexcept;
    double duration() const noexcept;

    // Codec name (e.g. "h264", "hevc", "vp9")
    const std::string& codec_name() const noexcept;

    // Bits per component of the decoded video (typically 8 or 10)
    int bit_depth() const noexcept;

    // --- Rotation ---

    // Rotation detected from the container's display matrix metadata.
    // This is the rotation that was applied by the recording device
    // (e.g. phone held in portrait mode records landscape + 90° tag).
    VideoRotation detected_rotation() const noexcept;

    // Effective rotation applied to decoded frames.  By default this
    // equals detected_rotation() (auto-rotate ON).  When auto-rotate
    // is disabled or a manual override is set, this reflects that.
    VideoRotation effective_rotation() const noexcept;

    // Enable/disable automatic rotation based on container metadata.
    // Default: enabled.  When disabled, frames are returned in coded
    // orientation (no rotation applied).
    void set_autorotate(bool enabled) noexcept;
    bool autorotate() const noexcept;

    // Set a manual rotation override.  When set, this replaces both
    // auto-rotation and any container metadata.  Pass VideoRotation::None
    // with autorotate=true to revert to metadata-based rotation.
    void set_manual_rotation(VideoRotation rotation) noexcept;
    VideoRotation manual_rotation() const noexcept;
    bool has_manual_rotation() const noexcept;
    void clear_manual_rotation() noexcept;

    // --- Decoding ---

    // Decode and return frame at the given index (0-based).
    // Returns an empty Mat on failure or out-of-range index.
    // The returned Mat is RGB24 (CV_8UC3), with rotation applied.
    //
    // Optimization:
    //   - If index == current position, returns cached frame (no decode).
    //   - If index == current + 1, decodes next frame sequentially.
    //   - Otherwise, seeks to nearest keyframe and decodes forward.
    cv::Mat decode_frame(int index);

    // Current decoded frame index, or -1 if no frame has been decoded yet.
    int current_frame_index() const noexcept;

    // Last error message (empty if no error).
    const std::string& last_error() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace idiff

#endif // IDIFF_HAVE_FFMPEG
#endif // IDIFF_VIDEO_DECODER_H
