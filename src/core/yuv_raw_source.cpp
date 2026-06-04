#ifdef IDIFF_HAVE_FFMPEG

#include "core/yuv_raw_source.h"
#include "core/video_filter_graph.h"
#include "core/image_impl.h"
#include "util/logger.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

#include <opencv2/core.hpp>

#include <cstring>
#include <filesystem>

namespace idiff {

// ============================================================================
// FFmpeg AVPixelFormat / AVColorSpace / AVColorPrimaries mapping
// ============================================================================

AVPixelFormat yuv_pixel_format_to_av(YuvPixelFormat f) noexcept {
    switch (f) {
        case YuvPixelFormat::YUV420P:   return AV_PIX_FMT_YUV420P;
        case YuvPixelFormat::YUV422P:   return AV_PIX_FMT_YUV422P;
        case YuvPixelFormat::YUV444P:   return AV_PIX_FMT_YUV444P;
        case YuvPixelFormat::YUV420P10: return AV_PIX_FMT_YUV420P10LE;
        case YuvPixelFormat::YUV422P10: return AV_PIX_FMT_YUV422P10LE;
        case YuvPixelFormat::YUV444P10: return AV_PIX_FMT_YUV444P10LE;
        case YuvPixelFormat::P010:      return AV_PIX_FMT_P010LE;
        case YuvPixelFormat::NV16:      return AV_PIX_FMT_NV16;
    }
    return AV_PIX_FMT_NONE;
}

AVColorSpace yuv_color_matrix_to_av(YuvColorMatrix m) noexcept {
    switch (m) {
        case YuvColorMatrix::BT601:      return AVCOL_SPC_SMPTE170M;
        case YuvColorMatrix::BT709:      return AVCOL_SPC_BT709;
        case YuvColorMatrix::BT2020_NCL: return AVCOL_SPC_BT2020_NCL;
    }
    return AVCOL_SPC_UNSPECIFIED;
}

AVColorPrimaries yuv_color_primaries_to_av(YuvColorPrimaries p) noexcept {
    switch (p) {
        case YuvColorPrimaries::BT601:  return AVCOL_PRI_SMPTE170M;
        case YuvColorPrimaries::BT709:  return AVCOL_PRI_BT709;
        case YuvColorPrimaries::BT2020: return AVCOL_PRI_BT2020;
    }
    return AVCOL_PRI_UNSPECIFIED;
}

// ============================================================================
// YuvRawSource::FfCtx -- FFmpeg demuxer/decoder state
// ============================================================================

struct YuvRawSource::FfCtx {
    AVFormatContext* fmt_ctx = nullptr;
    AVCodecContext* codec_ctx = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    int video_stream_idx = -1;
    int current_frame_idx = -1;

    // Display filter graph: YUV (native pixel format) -> sRGB RGB24.
    VideoFilterGraph display_graph;

    void close() noexcept {
        display_graph.reset();
        if (frame) av_frame_free(&frame);
        if (packet) av_packet_free(&packet);
        if (codec_ctx) avcodec_free_context(&codec_ctx);
        if (fmt_ctx) avformat_close_input(&fmt_ctx);
        video_stream_idx = -1;
        current_frame_idx = -1;
    }
};

// ============================================================================
// YuvRawSource
// ============================================================================

YuvRawSource::YuvRawSource(std::string path, const YuvStreamParams& params)
    : path_(std::move(path)), params_(params) {
    frame_bytes_ = yuv_frame_size_bytes(params_);
    frame_count_ = 0;
    if (frame_bytes_ > 0) {
        std::error_code ec;
        auto size = std::filesystem::file_size(path_, ec);
        if (!ec && size >= frame_bytes_) {
            frame_count_ = static_cast<int>(size / frame_bytes_);
        }
    }

    format_desc_ = std::string(yuv_pixel_format_name(params_.pixel_format)) + " "
                 + std::to_string(params_.width) + "x"
                 + std::to_string(params_.height) + " "
                 + std::to_string(yuv_pixel_format_bit_depth(params_.pixel_format))
                 + "-bit "
                 + yuv_color_matrix_name(params_.color_matrix) + " "
                 + yuv_color_range_name(params_.color_range);
}

YuvRawSource::~YuvRawSource() {
    close_ffmpeg();
}

bool YuvRawSource::open_ffmpeg() {
    close_ffmpeg();
    ff_ = std::make_unique<FfCtx>();

    const AVInputFormat* raw_fmt = av_find_input_format("rawvideo");
    if (!raw_fmt) {
        last_error_ = "rawvideo demuxer not found";
        return false;
    }

    // Build format options for the rawvideo demuxer.
    AVDictionary* opts = nullptr;
    char size_str[32];
    std::snprintf(size_str, sizeof(size_str), "%dx%d",
                  params_.width, params_.height);
    av_dict_set(&opts, "video_size", size_str, 0);

    const char* pix_fmt_name = av_get_pix_fmt_name(
        yuv_pixel_format_to_av(params_.pixel_format));
    if (pix_fmt_name) {
        av_dict_set(&opts, "pixel_format", pix_fmt_name, 0);
    }

    // rawvideo needs a framerate; the exact value does not matter for
    // decoding, but 0/0 causes some FFmpeg versions to refuse open.
    av_dict_set(&opts, "framerate", "25", 0);

    int ret = avformat_open_input(&ff_->fmt_ctx, path_.c_str(), raw_fmt, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        char err_buf[256];
        av_strerror(ret, err_buf, sizeof(err_buf));
        last_error_ = std::string("cannot open raw YUV file: ") + err_buf;
        close_ffmpeg();
        return false;
    }

    ret = avformat_find_stream_info(ff_->fmt_ctx, nullptr);
    if (ret < 0) {
        last_error_ = "cannot find stream info in raw YUV file";
        close_ffmpeg();
        return false;
    }

    // Find the video stream.
    for (unsigned int i = 0; i < ff_->fmt_ctx->nb_streams; ++i) {
        if (ff_->fmt_ctx->streams[i]->codecpar->codec_type ==
            AVMEDIA_TYPE_VIDEO) {
            ff_->video_stream_idx = static_cast<int>(i);
            break;
        }
    }
    if (ff_->video_stream_idx < 0) {
        last_error_ = "no video stream found in raw YUV file";
        close_ffmpeg();
        return false;
    }

    // Open the decoder (rawvideo "codec" just passes bytes through).
    AVStream* vstream = ff_->fmt_ctx->streams[ff_->video_stream_idx];
    const AVCodec* codec = avcodec_find_decoder(vstream->codecpar->codec_id);
    if (!codec) {
        last_error_ = "rawvideo decoder not found";
        close_ffmpeg();
        return false;
    }

    ff_->codec_ctx = avcodec_alloc_context3(codec);
    if (!ff_->codec_ctx) {
        last_error_ = "cannot allocate codec context";
        close_ffmpeg();
        return false;
    }

    ret = avcodec_parameters_to_context(ff_->codec_ctx, vstream->codecpar);
    if (ret < 0) {
        last_error_ = "cannot copy codec parameters";
        close_ffmpeg();
        return false;
    }

    // Inject the user-specified color metadata so the decoded frames
    // carry the correct tags for VideoFilterGraph.
    ff_->codec_ctx->color_range =
        (params_.color_range == YuvColorRange::Full)
            ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    ff_->codec_ctx->colorspace =
        yuv_color_matrix_to_av(params_.color_matrix);
    ff_->codec_ctx->color_primaries =
        yuv_color_primaries_to_av(params_.color_primaries);
    ff_->codec_ctx->color_trc = AVCOL_TRC_BT709;

    ret = avcodec_open2(ff_->codec_ctx, codec, nullptr);
    if (ret < 0) {
        char err_buf[256];
        av_strerror(ret, err_buf, sizeof(err_buf));
        last_error_ = std::string("cannot open codec: ") + err_buf;
        close_ffmpeg();
        return false;
    }

    ff_->frame = av_frame_alloc();
    ff_->packet = av_packet_alloc();
    if (!ff_->frame || !ff_->packet) {
        last_error_ = "cannot allocate AVFrame/AVPacket";
        close_ffmpeg();
        return false;
    }

    ff_->current_frame_idx = -1;
    last_error_.clear();
    return true;
}

void YuvRawSource::close_ffmpeg() noexcept {
    if (ff_) {
        ff_->close();
        ff_.reset();
    }
    ff_current_idx_ = -1;
}

// Decode the next frame from the rawvideo stream.  Returns true on
// success and advances ff_->current_frame_idx.
bool YuvRawSource::decode_next_frame() {
    while (true) {
        int ret = avcodec_receive_frame(ff_->codec_ctx, ff_->frame);
        if (ret == 0) {
            ff_->current_frame_idx++;
            return true;
        }
        if (ret == AVERROR_EOF) return false;
        if (ret != AVERROR(EAGAIN)) return false;

        while (true) {
            ret = av_read_frame(ff_->fmt_ctx, ff_->packet);
            if (ret < 0) {
                avcodec_send_packet(ff_->codec_ctx, nullptr);
                ret = avcodec_receive_frame(ff_->codec_ctx, ff_->frame);
                if (ret == 0) {
                    ff_->current_frame_idx++;
                    return true;
                }
                return false;
            }
            if (ff_->packet->stream_index == ff_->video_stream_idx) {
                ret = avcodec_send_packet(ff_->codec_ctx, ff_->packet);
                av_packet_unref(ff_->packet);
                if (ret < 0) return false;
                break;
            }
            av_packet_unref(ff_->packet);
        }
    }
}

std::unique_ptr<Image> YuvRawSource::read_frame(int index) {
    if (frame_bytes_ == 0) {
        last_error_ = "invalid YUV parameters";
        return nullptr;
    }
    if (index < 0 || index >= frame_count_) {
        last_error_ = "frame index out of range";
        return nullptr;
    }

    // Lazy-open FFmpeg on first access.
    if (!ff_) {
        if (!open_ffmpeg()) return nullptr;
    }

    // rawvideo is intra-only.  To seek to frame N we seek back to the
    // beginning and decode forward -- simple and always correct since
    // every frame is a keyframe.
    if (ff_->current_frame_idx != index) {
        // Seek to beginning.
        avcodec_flush_buffers(ff_->codec_ctx);
        int ret = av_seek_frame(ff_->fmt_ctx, ff_->video_stream_idx,
                                0, AVSEEK_FLAG_BACKWARD);
        if (ret < 0) {
            ret = avformat_seek_file(ff_->fmt_ctx, ff_->video_stream_idx,
                                     INT64_MIN, 0, INT64_MAX, 0);
        }
        if (ret < 0) {
            last_error_ = "seek failed in raw YUV file";
            return nullptr;
        }
        ff_->current_frame_idx = -1;

        // Decode forward to the target frame.
        while (ff_->current_frame_idx < index) {
            if (!decode_next_frame()) {
                last_error_ = "decode failed in raw YUV file";
                return nullptr;
            }
        }
    }

    // ff_->frame now holds the requested frame in native pixel format.
    // Configure or reuse the display filter graph.
    if (ff_->display_graph.needs_reconfigure(ff_->frame)) {
        const AVPixFmtDescriptor* pix_desc =
            av_pix_fmt_desc_get(static_cast<AVPixelFormat>(ff_->frame->format));
        int src_bit_depth = (pix_desc && pix_desc->nb_components > 0)
                                ? pix_desc->comp[0].depth
                                : 8;

        VideoFilterInputParams in;
        in.width     = ff_->frame->width;
        in.height    = ff_->frame->height;
        in.pix_fmt   = static_cast<AVPixelFormat>(ff_->frame->format);
        in.sar       = AVRational{1, 1};
        in.time_base = AVRational{1, 25};
        in.range     = (params_.color_range == YuvColorRange::Full)
                            ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
        in.matrix    = yuv_color_matrix_to_av(params_.color_matrix);
        in.primaries = yuv_color_primaries_to_av(params_.color_primaries);
        in.transfer  = AVCOL_TRC_BT709;

        VideoFilterOutputParams out;
        out.width     = 0;  // keep source dimensions
        out.height    = 0;
        out.pix_fmt   = AV_PIX_FMT_RGB24;
        out.range     = AVCOL_RANGE_JPEG;
        out.matrix    = AVCOL_SPC_RGB;
        out.primaries = AVCOL_PRI_BT709;
        out.transfer  = AVCOL_TRC_IEC61966_2_1;

        std::string err;
        if (!ff_->display_graph.configure(in, out, err)) {
            last_error_ = "display graph configure failed: " + err;
            return nullptr;
        }
    }

    // Push through the filter graph for YUV -> sRGB conversion.
    std::string err;
    AVFrame* rgb_frame = ff_->display_graph.process(ff_->frame, err);
    if (!rgb_frame) {
        last_error_ = "display graph process failed: " + err;
        return nullptr;
    }

    // Copy RGB24 data into a cv::Mat.
    const int out_w = rgb_frame->width;
    const int out_h = rgb_frame->height;
    cv::Mat mat(out_h, out_w, CV_8UC3);
    for (int y = 0; y < out_h; ++y) {
        std::memcpy(mat.ptr(y),
                    rgb_frame->data[0] + y * rgb_frame->linesize[0],
                    static_cast<size_t>(out_w) * 3);
    }
    av_frame_free(&rgb_frame);

    // Clone the native AVFrame for the pixel inspector.
    AVFrame* src_frame = av_frame_clone(ff_->frame);

    auto img = std::make_unique<Image>();
    img->internal().mat = mat;
    img->internal().src_av_frame = src_frame;
    img->internal().info.width = mat.cols;
    img->internal().info.height = mat.rows;
    img->internal().info.pixel_format = PixelFormat::RGB8;
    img->internal().info.source_format = SourceFormat::Unknown;
    img->internal().info.bit_depth = 8;
    img->internal().info.source_bit_depth =
        yuv_pixel_format_bit_depth(params_.pixel_format);
    img->internal().info.has_alpha = false;
    img->internal().info.color_space =
        std::string(yuv_color_matrix_name(params_.color_matrix)) + " "
        + yuv_color_range_name(params_.color_range);

    ff_current_idx_ = index;
    last_error_.clear();
    return img;
}

} // namespace idiff

#endif // IDIFF_HAVE_FFMPEG
