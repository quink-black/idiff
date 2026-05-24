#ifdef IDIFF_HAVE_FFMPEG

#include "core/video_decoder.h"
#include "util/logger.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/display.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace idiff {

// ============================================================================
// Impl
// ============================================================================

struct VideoDecoder::Impl {
    AVFormatContext* fmt_ctx = nullptr;
    AVCodecContext* codec_ctx = nullptr;
    SwsContext* sws_ctx = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* rgb_frame = nullptr;
    AVPacket* packet = nullptr;

    int video_stream_idx = -1;
    int coded_width = 0;
    int coded_height = 0;
    int frame_count = 0;
    int bit_depth = 8;
    double fps = 0.0;
    double duration = 0.0;
    std::string codec_name;
    std::string last_error;

    // Rotation
    VideoRotation detected_rotation = VideoRotation::None;
    bool autorotate = true;
    bool has_manual_rotation = false;
    VideoRotation manual_rotation = VideoRotation::None;

    // Decoding state
    int current_frame_idx = -1;  // index of the last successfully decoded frame
    cv::Mat cached_frame;        // cached RGB Mat of current_frame_idx
    VideoRotation cached_rotation = VideoRotation::None;  // rotation applied to cached_frame

    // RGB conversion buffer (allocated once, reused)
    std::vector<uint8_t> rgb_buffer;

    // Stream time_base for PTS/seek calculations
    AVRational time_base{0, 1};

    bool decode_next_frame();
    bool seek_to_frame(int target);
    cv::Mat seek_keyframe(int index);
    cv::Mat frame_to_mat();  // raw decoded frame (no rotation)
    cv::Mat apply_rotation(const cv::Mat& mat, VideoRotation rot);
    VideoRotation current_effective_rotation() const;
};

// Determine the effective rotation to apply.
VideoRotation VideoDecoder::Impl::current_effective_rotation() const {
    if (has_manual_rotation) return manual_rotation;
    if (autorotate) return detected_rotation;
    return VideoRotation::None;
}

// Apply rotation to a cv::Mat using OpenCV operations.
// For 90/180/270 this uses cv::rotate which is very efficient.
cv::Mat VideoDecoder::Impl::apply_rotation(const cv::Mat& mat, VideoRotation rot) {
    if (mat.empty() || rot == VideoRotation::None) return mat;

    cv::Mat result;
    switch (rot) {
        case VideoRotation::CW90:
            cv::rotate(mat, result, cv::ROTATE_90_CLOCKWISE);
            break;
        case VideoRotation::CW180:
            cv::rotate(mat, result, cv::ROTATE_180);
            break;
        case VideoRotation::CW270:
            cv::rotate(mat, result, cv::ROTATE_90_COUNTERCLOCKWISE);
            break;
        default:
            return mat;
    }
    return result;
}

// Decode the next frame from the stream.  Returns true if a video frame
// was successfully received.  Updates current_frame_idx on success.
bool VideoDecoder::Impl::decode_next_frame() {
    while (true) {
        // Try to receive a frame from the decoder first (handles buffered frames)
        int ret = avcodec_receive_frame(codec_ctx, frame);
        if (ret == 0) {
            // Got a frame
            current_frame_idx++;
            return true;
        }
        if (ret == AVERROR_EOF) {
            return false;
        }
        if (ret != AVERROR(EAGAIN)) {
            last_error = "avcodec_receive_frame failed";
            return false;
        }

        // Need more data — read packets until we find one for our stream
        while (true) {
            ret = av_read_frame(fmt_ctx, packet);
            if (ret < 0) {
                // End of file or error — flush the decoder
                avcodec_send_packet(codec_ctx, nullptr);
                ret = avcodec_receive_frame(codec_ctx, frame);
                if (ret == 0) {
                    current_frame_idx++;
                    return true;
                }
                return false;
            }

            if (packet->stream_index == video_stream_idx) {
                ret = avcodec_send_packet(codec_ctx, packet);
                av_packet_unref(packet);
                if (ret < 0) {
                    last_error = "avcodec_send_packet failed";
                    return false;
                }
                break;  // Go back to receive_frame
            }
            av_packet_unref(packet);
        }
    }
}

// Seek to a target frame index.  Seeks to the nearest keyframe before
// the target, then decodes forward.  Returns true if the target frame
// was reached.
bool VideoDecoder::Impl::seek_to_frame(int target) {
    if (target < 0 || target >= frame_count) {
        last_error = "frame index out of range";
        return false;
    }

    // For short seeks backward or to the beginning, always seek to start
    // and decode forward.  This is the most reliable approach for short
    // videos and avoids PTS estimation issues with B-frames.
    //
    // For longer videos, we could optimize by seeking to a keyframe near
    // the target, but for correctness we always seek to the start and
    // count frames.  This ensures frame indices are always accurate
    // regardless of B-frame reordering or variable frame durations.

    int ret = av_seek_frame(fmt_ctx, video_stream_idx, 0, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        // Try format-level seek as fallback
        ret = avformat_seek_file(fmt_ctx, video_stream_idx,
                                 INT64_MIN, 0, INT64_MAX, 0);
        if (ret < 0) {
            last_error = "seek to beginning failed";
            return false;
        }
    }

    // Flush decoder buffers after seek
    avcodec_flush_buffers(codec_ctx);
    current_frame_idx = -1;

    // Decode forward from the beginning until we reach the target frame
    while (current_frame_idx < target) {
        if (!decode_next_frame()) {
            last_error = "failed to reach target frame during forward decode";
            return false;
        }
    }

    return true;
}

// Seek to the nearest keyframe at or before the estimated PTS of
// frame `index`, decode only that keyframe, and return it.
// Does not update current_frame_idx -- this is a preview-only path.
cv::Mat VideoDecoder::Impl::seek_keyframe(int index) {
    if (index < 0 || index >= frame_count || !fmt_ctx || !codec_ctx)
        return {};

    // Estimate PTS from frame index + fps, then convert to stream
    // time_base.  This is approximate -- we only need to land on a
    // nearby keyframe, not hit the exact frame.
    double pts_sec = (fps > 0.0) ? index / fps : 0.0;
    int64_t pts = static_cast<int64_t>(pts_sec / av_q2d(time_base));

    int ret = avformat_seek_file(fmt_ctx, video_stream_idx,
                                 INT64_MIN, pts, pts,
                                 AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        // Fallback: seek to beginning
        ret = avformat_seek_file(fmt_ctx, video_stream_idx,
                                 INT64_MIN, 0, INT64_MAX, 0);
        if (ret < 0) return {};
    }

    avcodec_flush_buffers(codec_ctx);

    // Decode packets until we get the first video frame (the keyframe).
    // Limit attempts to avoid runaway reads on corrupt streams.
    constexpr int kMaxAttempts = 64;
    for (int i = 0; i < kMaxAttempts; ++i) {
        int r = avcodec_receive_frame(codec_ctx, frame);
        if (r == 0) {
            return frame_to_mat();
        }
        if (r == AVERROR_EOF) break;
        if (r != AVERROR(EAGAIN)) break;

        while (true) {
            r = av_read_frame(fmt_ctx, packet);
            if (r < 0) {
                avcodec_send_packet(codec_ctx, nullptr);
                r = avcodec_receive_frame(codec_ctx, frame);
                if (r == 0) return frame_to_mat();
                return {};
            }
            if (packet->stream_index == video_stream_idx) {
                r = avcodec_send_packet(codec_ctx, packet);
                av_packet_unref(packet);
                if (r < 0) return {};
                break;
            }
            av_packet_unref(packet);
        }
    }
    return {};
}

// Convert the current AVFrame to an RGB cv::Mat (no rotation applied).
cv::Mat VideoDecoder::Impl::frame_to_mat() {
    if (!frame || !sws_ctx) return {};

    sws_scale(sws_ctx,
              frame->data, frame->linesize, 0, coded_height,
              rgb_frame->data, rgb_frame->linesize);

    // Copy to cv::Mat (RGB, 8-bit, 3 channels)
    cv::Mat mat(coded_height, coded_width, CV_8UC3);
    for (int y = 0; y < coded_height; ++y) {
        std::memcpy(mat.ptr(y),
                    rgb_frame->data[0] + y * rgb_frame->linesize[0],
                    static_cast<size_t>(coded_width) * 3);
    }
    return mat;
}

// ============================================================================
// Public API
// ============================================================================

VideoDecoder::VideoDecoder() : impl_(std::make_unique<Impl>()) {}

VideoDecoder::~VideoDecoder() {
    close();
}

bool VideoDecoder::open(const std::string& path) {
    close();

    int ret = avformat_open_input(&impl_->fmt_ctx, path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        char err_buf[256];
        av_strerror(ret, err_buf, sizeof(err_buf));
        impl_->last_error = std::string("cannot open file: ") + err_buf;
        return false;
    }

    ret = avformat_find_stream_info(impl_->fmt_ctx, nullptr);
    if (ret < 0) {
        impl_->last_error = "cannot find stream info";
        close();
        return false;
    }

    // Find the first video stream
    for (unsigned int i = 0; i < impl_->fmt_ctx->nb_streams; ++i) {
        AVStream* stream = impl_->fmt_ctx->streams[i];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            impl_->video_stream_idx = static_cast<int>(i);
            break;
        }
    }

    if (impl_->video_stream_idx < 0) {
        impl_->last_error = "no video stream found";
        close();
        return false;
    }

    AVStream* video_stream = impl_->fmt_ctx->streams[impl_->video_stream_idx];
    impl_->time_base = video_stream->time_base;

    // Open video decoder
    const AVCodec* codec = avcodec_find_decoder(video_stream->codecpar->codec_id);
    if (!codec) {
        impl_->last_error = "unsupported video codec";
        close();
        return false;
    }

    impl_->codec_ctx = avcodec_alloc_context3(codec);
    if (!impl_->codec_ctx) {
        impl_->last_error = "cannot allocate codec context";
        close();
        return false;
    }

    ret = avcodec_parameters_to_context(impl_->codec_ctx, video_stream->codecpar);
    if (ret < 0) {
        impl_->last_error = "cannot copy codec parameters";
        close();
        return false;
    }

    // Enable multi-threaded decoding
    impl_->codec_ctx->thread_count = 0;  // auto-detect

    ret = avcodec_open2(impl_->codec_ctx, codec, nullptr);
    if (ret < 0) {
        char err_buf[256];
        av_strerror(ret, err_buf, sizeof(err_buf));
        impl_->last_error = std::string("cannot open codec: ") + err_buf;
        close();
        return false;
    }

    // Extract metadata
    impl_->coded_width = impl_->codec_ctx->width;
    impl_->coded_height = impl_->codec_ctx->height;
    impl_->codec_name = codec->name ? codec->name : "unknown";

    // Detect rotation from stream display matrix metadata.
    // This is how phone-recorded MP4s signal portrait orientation.
    impl_->detected_rotation = VideoRotation::None;
    {
        const AVPacketSideData *sd = av_packet_side_data_get(
            video_stream->codecpar->coded_side_data,
            video_stream->codecpar->nb_coded_side_data,
            AV_PKT_DATA_DISPLAYMATRIX);
        if (sd && sd->size >= 9 * sizeof(int32_t)) {
            const int32_t* displaymatrix = reinterpret_cast<const int32_t*>(sd->data);
            double theta = -av_display_rotation_get(displaymatrix);
            // Normalize to [0, 360)
            theta -= 360.0 * std::floor(theta / 360.0 + 0.9 / 360.0);
            // Snap to nearest 90° increment
            if (std::fabs(theta - 90.0) < 2.0) {
                impl_->detected_rotation = VideoRotation::CW90;
            } else if (std::fabs(theta - 180.0) < 2.0) {
                impl_->detected_rotation = VideoRotation::CW180;
            } else if (std::fabs(theta - 270.0) < 2.0) {
                impl_->detected_rotation = VideoRotation::CW270;
            }
            if (impl_->detected_rotation != VideoRotation::None) {
                LOG_INFO("VideoDecoder: detected rotation %.1f° -> %d°",
                         theta, static_cast<int>(impl_->detected_rotation));
            }
        }
    }

    // Determine bit depth from pixel format
    const AVPixFmtDescriptor* pix_desc = av_pix_fmt_desc_get(impl_->codec_ctx->pix_fmt);
    if (pix_desc && pix_desc->nb_components > 0) {
        impl_->bit_depth = pix_desc->comp[0].depth;
    }

    // Calculate FPS
    AVRational fr = video_stream->avg_frame_rate;
    if (fr.num > 0 && fr.den > 0) {
        impl_->fps = av_q2d(fr);
    } else {
        fr = video_stream->r_frame_rate;
        impl_->fps = (fr.num > 0 && fr.den > 0) ? av_q2d(fr) : 25.0;
    }

    // Calculate duration
    if (video_stream->duration != AV_NOPTS_VALUE) {
        impl_->duration = video_stream->duration * av_q2d(video_stream->time_base);
    } else if (impl_->fmt_ctx->duration != AV_NOPTS_VALUE) {
        impl_->duration = impl_->fmt_ctx->duration / static_cast<double>(AV_TIME_BASE);
    }

    // Determine frame count
    if (video_stream->nb_frames > 0) {
        impl_->frame_count = static_cast<int>(video_stream->nb_frames);
    } else if (impl_->fps > 0.0 && impl_->duration > 0.0) {
        impl_->frame_count = static_cast<int>(impl_->duration * impl_->fps + 0.5);
    } else {
        // Last resort: set to a large number; will be corrected during decode
        impl_->frame_count = 1;
    }

    // Setup sws_scale for pixel format conversion to RGB24
    impl_->sws_ctx = sws_getContext(
        impl_->coded_width, impl_->coded_height, impl_->codec_ctx->pix_fmt,
        impl_->coded_width, impl_->coded_height, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );
    if (!impl_->sws_ctx) {
        impl_->last_error = "cannot create sws context";
        close();
        return false;
    }

    // Allocate frames
    impl_->frame = av_frame_alloc();
    impl_->rgb_frame = av_frame_alloc();
    impl_->packet = av_packet_alloc();

    if (!impl_->frame || !impl_->rgb_frame || !impl_->packet) {
        impl_->last_error = "cannot allocate AVFrame/AVPacket";
        close();
        return false;
    }

    // Setup RGB frame buffer
    int rgb_size = av_image_get_buffer_size(AV_PIX_FMT_RGB24, impl_->coded_width, impl_->coded_height, 1);
    impl_->rgb_buffer.resize(static_cast<size_t>(rgb_size));
    av_image_fill_arrays(
        impl_->rgb_frame->data, impl_->rgb_frame->linesize,
        impl_->rgb_buffer.data(), AV_PIX_FMT_RGB24,
        impl_->coded_width, impl_->coded_height, 1
    );

    impl_->current_frame_idx = -1;
    impl_->last_error.clear();

    impl_->cached_rotation = VideoRotation::None;

    LOG_INFO("VideoDecoder: opened '%s' (%dx%d, %s, %.2f fps, %d frames, rotation=%d°)",
             path.c_str(), impl_->coded_width, impl_->coded_height,
             impl_->codec_name.c_str(), impl_->fps, impl_->frame_count,
             static_cast<int>(impl_->detected_rotation));

    return true;
}

void VideoDecoder::close() {
    if (impl_->sws_ctx) {
        sws_freeContext(impl_->sws_ctx);
        impl_->sws_ctx = nullptr;
    }
    if (impl_->frame) {
        av_frame_free(&impl_->frame);
    }
    if (impl_->rgb_frame) {
        av_frame_free(&impl_->rgb_frame);
    }
    if (impl_->packet) {
        av_packet_free(&impl_->packet);
    }
    if (impl_->codec_ctx) {
        avcodec_free_context(&impl_->codec_ctx);
    }
    if (impl_->fmt_ctx) {
        avformat_close_input(&impl_->fmt_ctx);
    }

    impl_->video_stream_idx = -1;
    impl_->current_frame_idx = -1;
    impl_->cached_frame = cv::Mat();
    impl_->cached_rotation = VideoRotation::None;
    impl_->rgb_buffer.clear();
}

bool VideoDecoder::is_open() const noexcept {
    return impl_->fmt_ctx != nullptr && impl_->codec_ctx != nullptr;
}

int VideoDecoder::width() const noexcept {
    VideoRotation rot = impl_->current_effective_rotation();
    if (rot == VideoRotation::CW90 || rot == VideoRotation::CW270)
        return impl_->coded_height;
    return impl_->coded_width;
}
int VideoDecoder::height() const noexcept {
    VideoRotation rot = impl_->current_effective_rotation();
    if (rot == VideoRotation::CW90 || rot == VideoRotation::CW270)
        return impl_->coded_width;
    return impl_->coded_height;
}
int VideoDecoder::coded_width() const noexcept { return impl_->coded_width; }
int VideoDecoder::coded_height() const noexcept { return impl_->coded_height; }
int VideoDecoder::frame_count() const noexcept { return impl_->frame_count; }
double VideoDecoder::fps() const noexcept { return impl_->fps; }
double VideoDecoder::duration() const noexcept { return impl_->duration; }
const std::string& VideoDecoder::codec_name() const noexcept { return impl_->codec_name; }
int VideoDecoder::bit_depth() const noexcept { return impl_->bit_depth; }
int VideoDecoder::current_frame_index() const noexcept { return impl_->current_frame_idx; }
const std::string& VideoDecoder::last_error() const noexcept { return impl_->last_error; }

VideoRotation VideoDecoder::detected_rotation() const noexcept {
    return impl_->detected_rotation;
}

VideoRotation VideoDecoder::effective_rotation() const noexcept {
    return impl_->current_effective_rotation();
}

void VideoDecoder::set_autorotate(bool enabled) noexcept {
    impl_->autorotate = enabled;
    // Invalidate cache if rotation changed
    impl_->cached_frame = cv::Mat();
}

bool VideoDecoder::autorotate() const noexcept {
    return impl_->autorotate;
}

void VideoDecoder::set_manual_rotation(VideoRotation rotation) noexcept {
    impl_->has_manual_rotation = true;
    impl_->manual_rotation = rotation;
    // Invalidate cache since rotation changed
    impl_->cached_frame = cv::Mat();
}

VideoRotation VideoDecoder::manual_rotation() const noexcept {
    return impl_->manual_rotation;
}

bool VideoDecoder::has_manual_rotation() const noexcept {
    return impl_->has_manual_rotation;
}

void VideoDecoder::clear_manual_rotation() noexcept {
    impl_->has_manual_rotation = false;
    impl_->manual_rotation = VideoRotation::None;
    // Invalidate cache
    impl_->cached_frame = cv::Mat();
}

cv::Mat VideoDecoder::decode_frame(int index) {
    if (!is_open()) {
        impl_->last_error = "decoder not open";
        return {};
    }

    if (index < 0 || index >= impl_->frame_count) {
        impl_->last_error = "frame index out of range";
        return {};
    }

    const VideoRotation rot = impl_->current_effective_rotation();

    // Cache hit: return the already-decoded frame (if rotation hasn't changed)
    if (index == impl_->current_frame_idx && !impl_->cached_frame.empty()
        && impl_->cached_rotation == rot) {
        impl_->last_error.clear();
        return impl_->cached_frame.clone();
    }

    // If same frame but rotation changed, just re-apply rotation
    if (index == impl_->current_frame_idx && impl_->cached_rotation != rot) {
        cv::Mat raw = impl_->frame_to_mat();
        impl_->cached_frame = impl_->apply_rotation(raw, rot);
        impl_->cached_rotation = rot;
        impl_->last_error.clear();
        return impl_->cached_frame.clone();
    }

    // Sequential read optimization: if requesting the next frame, just decode it
    if (index == impl_->current_frame_idx + 1) {
        if (impl_->decode_next_frame()) {
            cv::Mat raw = impl_->frame_to_mat();
            impl_->cached_frame = impl_->apply_rotation(raw, rot);
            impl_->cached_rotation = rot;
            impl_->last_error.clear();
            return impl_->cached_frame.clone();
        }
        impl_->last_error = "failed to decode next frame";
        return {};
    }

    // Forward-decode optimization: if the target is ahead of current position
    // and within a reasonable distance, just decode forward without seeking.
    // This avoids the expensive seek-to-beginning for small forward jumps.
    static constexpr int kMaxForwardDecode = 30;  // ~1 second at 30fps
    if (index > impl_->current_frame_idx &&
        index - impl_->current_frame_idx <= kMaxForwardDecode) {
        while (impl_->current_frame_idx < index) {
            if (!impl_->decode_next_frame()) {
                impl_->last_error = "failed to decode forward to target frame";
                return {};
            }
        }
        cv::Mat raw = impl_->frame_to_mat();
        impl_->cached_frame = impl_->apply_rotation(raw, rot);
        impl_->cached_rotation = rot;
        impl_->last_error.clear();
        return impl_->cached_frame.clone();
    }

    // Random access: seek and decode forward
    if (!impl_->seek_to_frame(index)) {
        // last_error already set by seek_to_frame
        return {};
    }

    cv::Mat raw = impl_->frame_to_mat();
    impl_->cached_frame = impl_->apply_rotation(raw, rot);
    impl_->cached_rotation = rot;
    impl_->last_error.clear();
    return impl_->cached_frame.clone();
}

cv::Mat VideoDecoder::decode_keyframe(int index) {
    if (!is_open()) {
        impl_->last_error = "decoder not open";
        return {};
    }
    if (index < 0 || index >= impl_->frame_count) {
        impl_->last_error = "frame index out of range";
        return {};
    }

    const VideoRotation rot = impl_->current_effective_rotation();
    cv::Mat raw = impl_->seek_keyframe(index);
    if (raw.empty()) {
        impl_->last_error = "keyframe seek failed";
        return {};
    }
    impl_->last_error.clear();
    return impl_->apply_rotation(raw, rot);
}

} // namespace idiff

#endif // IDIFF_HAVE_FFMPEG
