#ifdef IDIFF_HAVE_FFMPEG

#include "core/video_decoder.h"
#include "core/video_filter_graph.h"
#include "util/logger.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/display.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
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
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;

    // vf_scale-based display path.  Lazily configured on the first
    // frame (we need the frame's actual color tags, not codecpar's
    // possibly-stale prediction) and reconfigured automatically when
    // any source property changes mid-stream.  Output is sRGB-encoded
    // RGB24: BT.709 primaries + IEC 61966-2-1 transfer + full range,
    // matching what SDL textures and OpenCV CV_8UC3 callers expect.
    VideoFilterGraph display_graph;

    // Reference-counted snapshot of the most recently decoded frame.
    // Maintained by snapshot_current_frame(): av_frame_ref() shares
    // the underlying buffers with `frame`, so as long as we hold this
    // snapshot the buffers stay alive even after the next
    // avcodec_receive_frame() reuses `frame`.  cached_av_frame_idx is
    // the frame number this snapshot represents, or -1 when empty.
    AVFrame* cached_av_frame = nullptr;
    int cached_av_frame_idx = -1;

    int video_stream_idx = -1;
    int coded_width = 0;
    int coded_height = 0;
    int frame_count = 0;
    int bit_depth = 8;
    double fps = 0.0;
    double duration = 0.0;
    std::string codec_name;
    std::string last_error;

    // Whether the one-shot "first frame color tags" diagnostic has
    // already been emitted for the currently open file.  Reset by
    // close().
    bool color_tags_logged = false;

    // Rotation
    VideoRotation detected_rotation = VideoRotation::None;
    bool autorotate = true;
    bool has_manual_rotation = false;
    VideoRotation manual_rotation = VideoRotation::None;

    // Decoding state
    int current_frame_idx = -1;  // index of the last successfully decoded frame
    cv::Mat cached_frame;        // cached RGB Mat of current_frame_idx
    VideoRotation cached_rotation = VideoRotation::None;  // rotation applied to cached_frame

    // Stream time_base for PTS/seek calculations
    AVRational time_base{0, 1};

    bool decode_next_frame();
    bool seek_to_frame(int target);
    cv::Mat seek_keyframe(int index);
    cv::Mat frame_to_mat();  // raw decoded frame (no rotation)
    cv::Mat apply_rotation(const cv::Mat& mat, VideoRotation rot);
    VideoRotation current_effective_rotation() const;

    // Refresh `cached_av_frame` to reference the data currently in
    // `frame`.  Cheap: bumps refcounts, no pixel copies.  Idempotent
    // for the same frame index.
    void snapshot_current_frame();
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
// Invalidates the decode position so the next decode_frame() call
// will perform a full seek rather than assuming sequential state.
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

    // Invalidate decode position: the format context and codec are now
    // at an unknown frame.  The next decode_frame() must perform a full
    // seek rather than assuming it can decode forward from the cached
    // position.
    current_frame_idx = -1;
    cached_frame = cv::Mat();
    cached_rotation = VideoRotation::None;

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

// Take a reference to the current `frame` and stash it as
// `cached_av_frame`, indexed by the current decode position.  Cheap:
// av_frame_ref() bumps refcounts on the buffers, no pixel copy.  The
// stashed reference keeps those buffers alive even after the next
// avcodec_receive_frame() reuses `frame`.  Idempotent for the same
// frame index.
void VideoDecoder::Impl::snapshot_current_frame() {
    if (!frame || !cached_av_frame) return;
    if (cached_av_frame_idx == current_frame_idx &&
        cached_av_frame->buf[0] != nullptr) {
        return;  // already snapped this index
    }
    av_frame_unref(cached_av_frame);
    if (av_frame_ref(cached_av_frame, frame) < 0) {
        // ref failure: leave snapshot empty so callers see a miss
        cached_av_frame_idx = -1;
        return;
    }
    cached_av_frame_idx = current_frame_idx;

    // Emit a one-shot diagnostic of the resolved color tags carried by
    // the very first decoded frame.  We deliberately read AVFrame here
    // rather than codecpar at open() time, because frames are the
    // ground truth: a single file may carry frames with different
    // color descriptions.  Subsequent frames may differ; callers that
    // care must inspect each frame via VideoDecoder::frame_color_tags().
    if (!color_tags_logged) {
        VideoColorTags tags;
        tags.range     = frame->color_range;
        tags.matrix    = frame->colorspace;
        tags.primaries = frame->color_primaries;
        tags.transfer  = frame->color_trc;
        const int w = coded_width;
        const int h = coded_height;
        const char* mn = av_color_space_name(tags.resolved_matrix(w, h));
        const char* pn = av_color_primaries_name(
            tags.resolved_primaries(w, h));
        const char* tn = av_color_transfer_name(
            tags.resolved_transfer(w, h));
        const char* rn = av_color_range_name(tags.resolved_range());
        LOG_INFO("VideoDecoder: first-frame color tags resolved to "
                 "matrix=%s primaries=%s transfer=%s range=%s hdr=%d "
                 "(raw matrix=%d primaries=%d transfer=%d range=%d)",
                 mn ? mn : "?", pn ? pn : "?",
                 tn ? tn : "?", rn ? rn : "?",
                 tags.is_hdr() ? 1 : 0,
                 static_cast<int>(tags.matrix),
                 static_cast<int>(tags.primaries),
                 static_cast<int>(tags.transfer),
                 static_cast<int>(tags.range));
        color_tags_logged = true;
    }
}

// Convert the current AVFrame to an sRGB cv::Mat (no rotation applied).
//
// Pushes `frame` through `display_graph` (buffer -> scale ->
// buffersink).  The graph is reconfigured lazily on the first frame
// and any time the frame's geometry, pixel format, SAR, or color
// tags change mid-stream.  Output is always RGB24 with BT.709
// primaries, IEC 61966-2-1 transfer, and full range -- i.e. ready
// for direct upload as an sRGB SDL/OpenGL texture.  HDR sources
// (PQ / HLG transfer) are tone-mapped by vf_scale at this step.
cv::Mat VideoDecoder::Impl::frame_to_mat() {
    if (!frame) return {};

    if (display_graph.needs_reconfigure(frame)) {
        const int w = coded_width;
        const int h = coded_height;

        VideoColorTags tags;
        tags.range     = frame->color_range;
        tags.matrix    = frame->colorspace;
        tags.primaries = frame->color_primaries;
        tags.transfer  = frame->color_trc;

        VideoFilterInputParams in;
        in.width     = frame->width;
        in.height    = frame->height;
        in.pix_fmt   = static_cast<AVPixelFormat>(frame->format);
        in.sar       = frame->sample_aspect_ratio.num
                           ? frame->sample_aspect_ratio
                           : AVRational{1, 1};
        in.time_base = time_base;
        in.range     = tags.resolved_range();
        in.matrix    = tags.resolved_matrix(w, h);
        in.primaries = tags.resolved_primaries(w, h);
        in.transfer  = tags.resolved_transfer(w, h);

        VideoFilterOutputParams out;
        out.width     = 0;  // keep source size
        out.height    = 0;
        out.pix_fmt   = AV_PIX_FMT_RGB24;
        out.range     = AVCOL_RANGE_JPEG;          // full range
        out.matrix    = AVCOL_SPC_RGB;             // identity (RGB)
        out.primaries = AVCOL_PRI_BT709;
        out.transfer  = AVCOL_TRC_IEC61966_2_1;    // sRGB

        std::string err;
        if (!display_graph.configure(in, out, err)) {
            last_error = "display graph configure failed: " + err;
            LOG_ERROR("VideoDecoder: %s", last_error.c_str());
            return {};
        }
    }

    std::string err;
    AVFrame* out_frame = display_graph.process(frame, err);
    if (!out_frame) {
        last_error = "display graph process failed: " + err;
        LOG_ERROR("VideoDecoder: %s", last_error.c_str());
        return {};
    }

    // Copy to cv::Mat (RGB, 8-bit, 3 channels).  vf_scale already
    // produced contiguous RGB24, but linesize may include padding,
    // so copy row by row.
    const int out_w = out_frame->width;
    const int out_h = out_frame->height;
    cv::Mat mat(out_h, out_w, CV_8UC3);
    for (int y = 0; y < out_h; ++y) {
        std::memcpy(mat.ptr(y),
                    out_frame->data[0] + y * out_frame->linesize[0],
                    static_cast<size_t>(out_w) * 3);
    }
    av_frame_free(&out_frame);
    return mat;
}

// ============================================================================
// VideoColorTags resolution
// ============================================================================
//
// FFmpeg's command-line tools and most decoders fall back to the same
// SD/HD/UHD heuristics when a stream signals UNSPECIFIED.  We mirror
// those rules here so vf_scale and our pixel inspector see concrete
// values without each caller reinventing the table.
//
// - SD (height <= 576): BT.601 / SMPTE170M everywhere
// - HD (height in (576, 1080]): BT.709
// - UHD-or-larger: BT.2020 NCL (gamut + matrix); transfer stays BT.709
//   unless an explicit PQ/HLG was already set
// - range: limited (MPEG) unless explicitly full

bool VideoColorTags::is_hdr() const noexcept {
    return transfer == AVCOL_TRC_SMPTE2084 ||
           transfer == AVCOL_TRC_ARIB_STD_B67;
}

AVColorRange VideoColorTags::resolved_range() const noexcept {
    if (range != AVCOL_RANGE_UNSPECIFIED) return range;
    return AVCOL_RANGE_MPEG;
}

namespace {

bool is_sd(int height) noexcept { return height > 0 && height <= 576; }
bool is_hd(int height) noexcept { return height > 576 && height <= 1080; }

} // namespace

AVColorSpace VideoColorTags::resolved_matrix(int /*width*/,
                                             int height) const noexcept {
    if (matrix != AVCOL_SPC_UNSPECIFIED) return matrix;
    if (is_sd(height)) return AVCOL_SPC_SMPTE170M;
    if (is_hd(height)) return AVCOL_SPC_BT709;
    // UHD or larger.  If the stream already advertises an HDR transfer,
    // BT.2020 is the only sensible default; otherwise still BT.709 since
    // a 4K SDR stream without tags is far more common than a 4K BT.2020
    // SDR stream in the wild.
    if (is_hdr()) {
        return AVCOL_SPC_BT2020_NCL;
    }
    return AVCOL_SPC_BT709;
}

AVColorPrimaries VideoColorTags::resolved_primaries(int /*width*/,
                                                    int height) const noexcept {
    if (primaries != AVCOL_PRI_UNSPECIFIED) return primaries;
    if (is_sd(height)) return AVCOL_PRI_SMPTE170M;
    if (is_hd(height)) return AVCOL_PRI_BT709;
    if (is_hdr()) {
        return AVCOL_PRI_BT2020;
    }
    return AVCOL_PRI_BT709;
}

AVColorTransferCharacteristic
VideoColorTags::resolved_transfer(int /*width*/, int height) const noexcept {
    if (transfer != AVCOL_TRC_UNSPECIFIED) return transfer;
    if (is_sd(height)) return AVCOL_TRC_SMPTE170M;
    return AVCOL_TRC_BT709;
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

    // Allocate frames.  The display path's filter graph is configured
    // lazily inside frame_to_mat() once we have a real AVFrame in
    // hand: codecpar's color description is at best a hint, and we
    // want vf_scale wired to the actual decoded frame's tags.
    impl_->frame = av_frame_alloc();
    impl_->cached_av_frame = av_frame_alloc();
    impl_->packet = av_packet_alloc();

    if (!impl_->frame || !impl_->cached_av_frame || !impl_->packet) {
        impl_->last_error = "cannot allocate AVFrame/AVPacket";
        close();
        return false;
    }

    impl_->current_frame_idx = -1;
    impl_->cached_av_frame_idx = -1;
    impl_->color_tags_logged = false;
    impl_->last_error.clear();

    impl_->cached_rotation = VideoRotation::None;

    LOG_INFO("VideoDecoder: opened '%s' (%dx%d, %s, %.2f fps, %d frames, rotation=%d°)",
             path.c_str(), impl_->coded_width, impl_->coded_height,
             impl_->codec_name.c_str(), impl_->fps, impl_->frame_count,
             static_cast<int>(impl_->detected_rotation));

    return true;
}

void VideoDecoder::close() {
    impl_->display_graph.reset();
    if (impl_->frame) {
        av_frame_free(&impl_->frame);
    }
    if (impl_->cached_av_frame) {
        av_frame_free(&impl_->cached_av_frame);
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
    impl_->cached_av_frame_idx = -1;
    impl_->color_tags_logged = false;
    impl_->cached_frame = cv::Mat();
    impl_->cached_rotation = VideoRotation::None;
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

VideoDecoder::SAR VideoDecoder::sar() const noexcept {
    if (!impl_->fmt_ctx || impl_->video_stream_idx < 0) return {0, 0};
    AVStream* st = impl_->fmt_ctx->streams[impl_->video_stream_idx];
    if (st->codecpar->sample_aspect_ratio.num && st->codecpar->sample_aspect_ratio.den) {
        return {st->codecpar->sample_aspect_ratio.num,
                st->codecpar->sample_aspect_ratio.den};
    }
    // Fall back to codec context SAR.
    if (impl_->codec_ctx &&
        impl_->codec_ctx->sample_aspect_ratio.num &&
        impl_->codec_ctx->sample_aspect_ratio.den) {
        return {impl_->codec_ctx->sample_aspect_ratio.num,
                impl_->codec_ctx->sample_aspect_ratio.den};
    }
    return {0, 0};
}

int VideoDecoder::current_frame_index() const noexcept { return impl_->current_frame_idx; }
const std::string& VideoDecoder::last_error() const noexcept { return impl_->last_error; }

VideoColorTags VideoDecoder::frame_color_tags() const noexcept {
    VideoColorTags tags;
    if (impl_->cached_av_frame &&
        impl_->cached_av_frame->buf[0] != nullptr) {
        const AVFrame* f = impl_->cached_av_frame;
        tags.range     = f->color_range;
        tags.matrix    = f->colorspace;
        tags.primaries = f->color_primaries;
        tags.transfer  = f->color_trc;
    }
    return tags;
}

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
        return impl_->cached_frame;
    }

    // If same frame but rotation changed, just re-apply rotation
    if (index == impl_->current_frame_idx && impl_->cached_rotation != rot) {
        cv::Mat raw = impl_->frame_to_mat();
        impl_->cached_frame = impl_->apply_rotation(raw, rot);
        impl_->cached_rotation = rot;
        impl_->last_error.clear();
        return impl_->cached_frame;
    }

    // Sequential read optimization: if requesting the next frame, just decode it
    if (index == impl_->current_frame_idx + 1) {
        if (impl_->decode_next_frame()) {
            impl_->snapshot_current_frame();
            cv::Mat raw = impl_->frame_to_mat();
            impl_->cached_frame = impl_->apply_rotation(raw, rot);
            impl_->cached_rotation = rot;
            impl_->last_error.clear();
            return impl_->cached_frame;
        }
        impl_->last_error = "failed to decode next frame";
        return {};
    }

    // Forward-decode optimization: if the target is ahead of current position
    // and within a reasonable distance, just decode forward without seeking.
    // This avoids the expensive seek-to-beginning for small forward jumps.
    // Skip when current_frame_idx is -1 (invalidated state) -- the stream
    // position is unknown and forward-decode would read from the wrong spot.
    static constexpr int kMaxForwardDecode = 30;  // ~1 second at 30fps
    if (impl_->current_frame_idx >= 0 &&
        index > impl_->current_frame_idx &&
        index - impl_->current_frame_idx <= kMaxForwardDecode) {
        while (impl_->current_frame_idx < index) {
            if (!impl_->decode_next_frame()) {
                impl_->last_error = "failed to decode forward to target frame";
                return {};
            }
        }
        impl_->snapshot_current_frame();
        cv::Mat raw = impl_->frame_to_mat();
        impl_->cached_frame = impl_->apply_rotation(raw, rot);
        impl_->cached_rotation = rot;
        impl_->last_error.clear();
        return impl_->cached_frame;
    }

    // Random access: seek and decode forward
    if (!impl_->seek_to_frame(index)) {
        // last_error already set by seek_to_frame
        return {};
    }

    impl_->snapshot_current_frame();
    cv::Mat raw = impl_->frame_to_mat();
    impl_->cached_frame = impl_->apply_rotation(raw, rot);
    impl_->cached_rotation = rot;
    impl_->last_error.clear();
    return impl_->cached_frame;
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

AVFrame* VideoDecoder::decode_frame_raw(int index) {
    if (!is_open()) {
        impl_->last_error = "decoder not open";
        return nullptr;
    }
    if (index < 0 || index >= impl_->frame_count) {
        impl_->last_error = "frame index out of range";
        return nullptr;
    }

    // If neither the cached snapshot nor the live decoder position is at
    // `index`, drive a full decode through decode_frame() so we exercise
    // the same caching, sequential-read and seek logic in exactly one
    // place.  decode_frame() takes the snapshot for us at every full
    // decode point, so on return cached_av_frame_idx == index.
    const bool snap_hit = (impl_->cached_av_frame_idx == index &&
                           impl_->cached_av_frame &&
                           impl_->cached_av_frame->buf[0] != nullptr);
    if (!snap_hit) {
        cv::Mat m = decode_frame(index);
        if (m.empty()) {
            // last_error already populated by decode_frame()
            return nullptr;
        }
        // decode_frame() at the cache-hit-with-same-rotation path does
        // not redecode and therefore does not snapshot.  Cover that
        // case by snapping here -- it's a no-op when the snapshot is
        // already current.
        impl_->snapshot_current_frame();
    }

    if (impl_->cached_av_frame_idx != index ||
        !impl_->cached_av_frame ||
        impl_->cached_av_frame->buf[0] == nullptr) {
        impl_->last_error = "raw frame snapshot unavailable";
        return nullptr;
    }

    AVFrame* out = av_frame_clone(impl_->cached_av_frame);
    if (!out) {
        impl_->last_error = "av_frame_clone failed";
        return nullptr;
    }
    impl_->last_error.clear();
    return out;
}

} // namespace idiff

#endif // IDIFF_HAVE_FFMPEG
