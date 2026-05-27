#ifndef IDIFF_VIDEO_DECODER_H
#define IDIFF_VIDEO_DECODER_H

#ifdef IDIFF_HAVE_FFMPEG

#include <cstdint>
#include <memory>
#include <string>

#include <opencv2/core.hpp>

extern "C" {
#include <libavutil/pixfmt.h>
}

// Forward-declare AVFrame in the global namespace so we can return a
// pointer to it from VideoDecoder::decode_frame_raw() without dragging
// <libavutil/frame.h> into every translation unit that includes this
// header.
struct AVFrame;

namespace idiff {

// Rotation mode for video frames.  Used both for auto-detected rotation
// from container metadata and for manual user override.
enum class VideoRotation {
    None = 0,      // No rotation
    CW90 = 90,     // 90° clockwise
    CW180 = 180,   // 180°
    CW270 = 270,   // 270° clockwise (= 90° counter-clockwise)
};

// Snapshot of the four color-description fields on a single AVFrame.
// These are *frame-level* values: in adversarial inputs (mid-stream
// codec switch, ABR rendition switch, multi-segment muxes) they can
// change frame to frame, so callers must obtain a fresh snapshot per
// frame rather than caching a single value across the file.
//
// All four members are stored verbatim from AVFrame, including the
// FFmpeg "UNSPECIFIED" sentinel.  resolved_*() applies FFmpeg's
// SD/HD/UHD-HDR fallbacks at the point where libswscale needs a
// concrete value.
struct VideoColorTags {
    AVColorRange range = AVCOL_RANGE_UNSPECIFIED;
    AVColorSpace matrix = AVCOL_SPC_UNSPECIFIED;
    AVColorPrimaries primaries = AVCOL_PRI_UNSPECIFIED;
    AVColorTransferCharacteristic transfer = AVCOL_TRC_UNSPECIFIED;

    // True iff the transfer characteristic is PQ (SMPTE2084) or HLG
    // (ARIB STD-B67).  Gamut alone is not enough to call a frame HDR;
    // BT.2020 with a BT.709 transfer is still SDR.  Computed from
    // `transfer` at call time, never stored.
    bool is_hdr() const noexcept;

    // Resolved values: identical to the raw fields when present, and
    // filled in with FFmpeg-style defaults when UNSPECIFIED.  These
    // are what callers should pass to libswscale / vf_scale.
    AVColorRange resolved_range() const noexcept;
    AVColorSpace resolved_matrix(int width, int height) const noexcept;
    AVColorPrimaries resolved_primaries(int width, int height) const noexcept;
    AVColorTransferCharacteristic resolved_transfer(int width,
                                                    int height) const noexcept;
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

    // Color tags read from the most recently decoded AVFrame.  Returned
    // by value because the underlying values are frame-level: a single
    // file may carry frames with different color descriptions (e.g.
    // ABR rendition switch, mid-stream codec switch).  Callers that
    // configure libswscale / vf_scale per frame must call this each
    // time `current_frame_index()` advances.
    //
    // Before any frame has been decoded successfully, all four members
    // are AVCOL_*_UNSPECIFIED.
    VideoColorTags frame_color_tags() const noexcept;

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

    // Decode the frame at `index` and return a reference-counted handle
    // to the underlying AVFrame in its native pixel format and bit
    // depth, *before* any conversion to RGB24.  Internally implemented
    // with av_frame_clone() (= av_frame_alloc + av_frame_ref): the
    // returned AVFrame shares its pixel buffers with the decoder's
    // cached frame; nothing is deep-copied.
    //
    // Caller responsibilities:
    //   - call av_frame_free() on the returned pointer when done; this
    //     releases just the caller's reference, not the underlying
    //     buffers.
    //   - call av_frame_make_writable() before mutating pixel data.
    //   - rotation is NOT applied here; apply it after pixel-format
    //     conversion if needed.
    //
    // Returns nullptr on failure (index out of range, decoder closed,
    // or decode error).  Sets last_error() on failure.
    //
    // Calling this with the same index that was last decoded by either
    // decode_frame() or decode_frame_raw() does not trigger a re-decode
    // -- a fresh reference to the cached frame is returned.
    struct ::AVFrame* decode_frame_raw(int index);

    // Fast approximate decode for scrubbing: seeks to the nearest keyframe
    // at or before the target timestamp and decodes only that keyframe.
    // The returned frame may not correspond to the exact index -- it is
    // the closest keyframe for preview purposes.  Does not update
    // current_frame_index() or the internal decode position.
    cv::Mat decode_keyframe(int index);

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
