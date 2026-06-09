// SPDX-License-Identifier: BSD-2-Clause
#ifdef IDIFF_HAVE_FFMPEG_IMAGE_DECODE

#include "core/detail/ffmpeg_image_loader.h"
#include "core/detail/heif_tile_assembler.h"
#include "core/image_impl.h"
#include "core/image_loader.h"  // for LoadFlag
#include "util/logger.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <opencv2/core.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace idiff {

namespace {

// ============================================================================
// Misc helpers
// ============================================================================

void set_av_err(std::string& err, const char* prefix, int ret) {
    char buf[128];
    av_strerror(ret, buf, sizeof(buf));
    err = std::string(prefix) + ": " + buf;
}

bool has_flag(std::uint32_t flags, LoadFlag flag) {
    return (flags & static_cast<std::uint32_t>(flag)) != 0;
}

// ============================================================================
// In-memory AVIO bridge
// ============================================================================
//
// Lets us hand libavformat a plain `(data, size)` buffer the same way
// the ImageMagick / OpenCV paths do, avoiding Windows non-ASCII path
// bugs and matching the decoder-from-memory contract used elsewhere
// in the codebase.

struct MemReader {
    const uint8_t* data;
    std::size_t size;
    std::size_t pos;
};

int memreader_read(void* opaque, uint8_t* buf, int buf_size) {
    auto* r = static_cast<MemReader*>(opaque);
    if (r->pos >= r->size) return AVERROR_EOF;
    std::size_t remaining = r->size - r->pos;
    int n = static_cast<int>(std::min<std::size_t>(remaining,
                                static_cast<std::size_t>(buf_size)));
    std::memcpy(buf, r->data + r->pos, n);
    r->pos += n;
    return n;
}

int64_t memreader_seek(void* opaque, int64_t offset, int whence) {
    auto* r = static_cast<MemReader*>(opaque);
    if (whence == AVSEEK_SIZE) {
        return static_cast<int64_t>(r->size);
    }
    int64_t target = 0;
    switch (whence) {
        case SEEK_SET: target = offset; break;
        case SEEK_CUR: target = static_cast<int64_t>(r->pos) + offset; break;
        case SEEK_END: target = static_cast<int64_t>(r->size) + offset; break;
        default: return -1;
    }
    if (target < 0 || target > static_cast<int64_t>(r->size)) {
        return -1;
    }
    r->pos = static_cast<std::size_t>(target);
    return target;
}

// ============================================================================
// RAII wrappers
// ============================================================================

struct AVIOContextDeleter {
    void operator()(AVIOContext* ctx) const noexcept {
        if (!ctx) return;
        // The reader buffer was allocated via av_malloc(); avio_context_free
        // does not free the user buffer, only the AVIOContext itself.
        if (ctx->buffer) av_freep(&ctx->buffer);
        avio_context_free(&ctx);
    }
};
using AVIOContextPtr = std::unique_ptr<AVIOContext, AVIOContextDeleter>;

struct AVFormatContextDeleter {
    void operator()(AVFormatContext* ctx) const noexcept {
        if (ctx) avformat_close_input(&ctx);
    }
};
using AVFormatContextPtr = std::unique_ptr<AVFormatContext, AVFormatContextDeleter>;

struct AVCodecContextDeleter {
    void operator()(AVCodecContext* ctx) const noexcept {
        if (ctx) avcodec_free_context(&ctx);
    }
};
using AVCodecContextPtr = std::unique_ptr<AVCodecContext, AVCodecContextDeleter>;

struct AVPacketDeleter {
    void operator()(AVPacket* p) const noexcept {
        if (p) av_packet_free(&p);
    }
};
using AVPacketPtr = std::unique_ptr<AVPacket, AVPacketDeleter>;

struct AVFrameDeleter {
    void operator()(AVFrame* f) const noexcept {
        if (f) av_frame_free(&f);
    }
};
using AVFramePtr = std::unique_ptr<AVFrame, AVFrameDeleter>;

struct SwsContextDeleter {
    void operator()(SwsContext* sws) const noexcept {
        if (sws) sws_freeContext(sws);
    }
};
using SwsContextPtr = std::unique_ptr<SwsContext, SwsContextDeleter>;

// ============================================================================
// Single-stream decode
// ============================================================================
//
// HEIF / AVIF stream groups expose every tile as its own AVStream.
// Each tile is a single still picture, so a single decode_packet +
// flush is enough to pull exactly one AVFrame.

AVFramePtr decode_one_frame(AVFormatContext* fmt_ctx,
                            int stream_index,
                            std::string& err) {
    AVStream* st = fmt_ctx->streams[stream_index];
    const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) {
        char buf[96];
        std::snprintf(buf, sizeof(buf),
                      "stream %d: no decoder for codec id %d",
                      stream_index, static_cast<int>(st->codecpar->codec_id));
        err = buf;
        return nullptr;
    }

    AVCodecContextPtr cctx(avcodec_alloc_context3(dec));
    if (!cctx) {
        err = "avcodec_alloc_context3 failed";
        return nullptr;
    }
    if (int ret = avcodec_parameters_to_context(cctx.get(), st->codecpar);
        ret < 0) {
        set_av_err(err, "avcodec_parameters_to_context failed", ret);
        return nullptr;
    }
    if (int ret = avcodec_open2(cctx.get(), dec, nullptr); ret < 0) {
        char prefix[64];
        std::snprintf(prefix, sizeof(prefix),
                      "stream %d: avcodec_open2 failed", stream_index);
        set_av_err(err, prefix, ret);
        return nullptr;
    }

    AVPacketPtr pkt(av_packet_alloc());
    AVFramePtr frame(av_frame_alloc());
    if (!pkt || !frame) {
        err = "packet/frame alloc failed";
        return nullptr;
    }

    // Rewind so we don't miss any earlier tile when called sequentially
    // for multiple streams in the same fmt_ctx -- demuxer position is
    // shared across streams.
    av_seek_frame(fmt_ctx, stream_index, 0, AVSEEK_FLAG_BYTE);

    bool got_frame = false;
    while (!got_frame) {
        int ret = av_read_frame(fmt_ctx, pkt.get());
        if (ret == AVERROR_EOF) {
            // EOF: send NULL to flush any buffered output.
            avcodec_send_packet(cctx.get(), nullptr);
        } else if (ret < 0) {
            set_av_err(err, "av_read_frame failed", ret);
            return nullptr;
        } else if (pkt->stream_index != stream_index) {
            av_packet_unref(pkt.get());
            continue;
        } else {
            ret = avcodec_send_packet(cctx.get(), pkt.get());
            av_packet_unref(pkt.get());
            if (ret < 0) {
                set_av_err(err, "avcodec_send_packet failed", ret);
                return nullptr;
            }
        }

        ret = avcodec_receive_frame(cctx.get(), frame.get());
        if (ret == 0) {
            got_frame = true;
            break;
        }
        if (ret == AVERROR(EAGAIN)) {
            continue;
        }
        if (ret == AVERROR_EOF) {
            err = "decoder produced no frame";
            return nullptr;
        }
        set_av_err(err, "avcodec_receive_frame failed", ret);
        return nullptr;
    }

    return frame;
}

// ============================================================================
// AVFrame -> Image (RGB(A) 8 / 16-bit)
// ============================================================================

PixelFormat select_target_pixel_format(AVPixelFormat src,
                                       std::uint32_t flags,
                                       AVPixelFormat& out_av_fmt) {
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(src);
    bool src_has_alpha = desc && (desc->flags & AV_PIX_FMT_FLAG_ALPHA);
    bool src_high_depth = false;
    if (desc && desc->nb_components > 0) {
        src_high_depth = desc->comp[0].depth > 8;
    }

    bool keep_alpha = has_flag(flags, LoadFlag::KeepAlpha) && src_has_alpha;
    bool keep_16    = has_flag(flags, LoadFlag::Keep16Bit) && src_high_depth;

    if (keep_16 && keep_alpha) {
        out_av_fmt = AV_PIX_FMT_RGBA64LE;
        return PixelFormat::RGBA16;
    }
    if (keep_16) {
        out_av_fmt = AV_PIX_FMT_RGB48LE;
        return PixelFormat::RGB16;
    }
    if (keep_alpha) {
        out_av_fmt = AV_PIX_FMT_RGBA;
        return PixelFormat::RGBA8;
    }
    out_av_fmt = AV_PIX_FMT_RGB24;
    return PixelFormat::RGB8;
}

int pixel_format_channels(PixelFormat fmt) {
    switch (fmt) {
        case PixelFormat::Gray8:
        case PixelFormat::Gray16:  return 1;
        case PixelFormat::RGB8:
        case PixelFormat::RGB16:   return 3;
        case PixelFormat::RGBA8:
        case PixelFormat::RGBA16:  return 4;
    }
    return 3;
}

bool avframe_to_image(AVFrame* frame,
                      Image& image,
                      SourceFormat src_format,
                      std::uint32_t flags,
                      bool icc_profile_present,
                      std::string& err) {
    AVPixelFormat target_av_fmt = AV_PIX_FMT_NONE;
    PixelFormat target_fmt = select_target_pixel_format(
        static_cast<AVPixelFormat>(frame->format), flags, target_av_fmt);

    int channels = pixel_format_channels(target_fmt);
    int bit_depth = (target_fmt == PixelFormat::RGB16 ||
                     target_fmt == PixelFormat::RGBA16) ? 16 : 8;
    int cv_type = (bit_depth == 16) ? CV_16UC(channels) : CV_8UC(channels);

    cv::Mat mat(frame->height, frame->width, cv_type);
    if (mat.empty()) {
        err = "cv::Mat allocation failed";
        return false;
    }

    SwsContextPtr sws(sws_getContext(
        frame->width, frame->height,
        static_cast<AVPixelFormat>(frame->format),
        frame->width, frame->height,
        target_av_fmt,
        SWS_BILINEAR, nullptr, nullptr, nullptr));
    if (!sws) {
        err = "sws_getContext failed";
        return false;
    }

    uint8_t* dst_data[4]   = {mat.ptr(), nullptr, nullptr, nullptr};
    int      dst_strides[4] = {static_cast<int>(mat.step[0]), 0, 0, 0};
    int rc = sws_scale(sws.get(),
                       frame->data, frame->linesize,
                       0, frame->height,
                       dst_data, dst_strides);
    if (rc <= 0) {
        err = "sws_scale produced no output";
        return false;
    }

    auto& impl = image.internal();
    impl.info.width            = frame->width;
    impl.info.height           = frame->height;
    impl.info.pixel_format     = target_fmt;
    impl.info.bit_depth        = bit_depth;
    impl.info.has_alpha        = (channels == 4);
    impl.info.color_space      = "sRGB";
    impl.info.source_format    = src_format;

    // Record the source bit depth so the UI can show "10-bit AVIF"
    // even when we tone-mapped down to RGB8.
    const AVPixFmtDescriptor* src_desc =
        av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
    if (src_desc && src_desc->nb_components > 0) {
        impl.info.source_bit_depth = src_desc->comp[0].depth;
    }

    if (icc_profile_present) {
        impl.info.icc_profile_name = "Embedded ICC";
    }

    // Propagate SAR from the source frame.
    if (frame->sample_aspect_ratio.num && frame->sample_aspect_ratio.den) {
        impl.info.sar_num = frame->sample_aspect_ratio.num;
        impl.info.sar_den = frame->sample_aspect_ratio.den;
    }

    impl.mat = std::move(mat);
    return true;
}

// ============================================================================
// ICC profile detection
// ============================================================================

bool stream_group_has_icc(const AVStreamGroup* stg) {
    if (!stg || stg->type != AV_STREAM_GROUP_PARAMS_TILE_GRID) return false;
    const AVStreamGroupTileGrid* grid = stg->params.tile_grid;
    if (!grid) return false;
    for (int i = 0; i < grid->nb_coded_side_data; ++i) {
        if (grid->coded_side_data[i].type == AV_PKT_DATA_ICC_PROFILE) {
            return true;
        }
    }
    return false;
}

bool stream_has_icc(const AVStream* st) {
    if (!st || !st->codecpar) return false;
    for (int i = 0; i < st->codecpar->nb_coded_side_data; ++i) {
        if (st->codecpar->coded_side_data[i].type == AV_PKT_DATA_ICC_PROFILE) {
            return true;
        }
    }
    return false;
}

bool frame_has_icc(const AVFrame* f) {
    if (!f) return false;
    for (int i = 0; i < f->nb_side_data; ++i) {
        if (f->side_data[i]->type == AV_FRAME_DATA_ICC_PROFILE) {
            return true;
        }
    }
    return false;
}

// ============================================================================
// Multi-tile vs single-tile dispatch
// ============================================================================

const AVStreamGroup* find_tile_grid_group(AVFormatContext* fmt_ctx) {
    for (unsigned int i = 0; i < fmt_ctx->nb_stream_groups; ++i) {
        AVStreamGroup* stg = fmt_ctx->stream_groups[i];
        if (stg && stg->type == AV_STREAM_GROUP_PARAMS_TILE_GRID) {
            return stg;
        }
    }
    return nullptr;
}

std::unique_ptr<Image> decode_tile_grid(AVFormatContext* fmt_ctx,
                                        const AVStreamGroup* stg,
                                        SourceFormat src_format,
                                        std::uint32_t flags,
                                        std::string& err) {
    const AVStreamGroupTileGrid* grid = stg->params.tile_grid;
    if (!grid || grid->nb_tiles == 0) {
        err = "tile grid is empty";
        return nullptr;
    }

    if (grid->nb_tiles != stg->nb_streams) {
        char buf[96];
        std::snprintf(buf, sizeof(buf),
                      "tile grid: nb_tiles %u != group->nb_streams %u",
                      grid->nb_tiles,
                      static_cast<unsigned>(stg->nb_streams));
        err = buf;
        return nullptr;
    }

    // ---- decode every tile to an AVFrame ----
    std::vector<AVFramePtr> tile_frames;
    tile_frames.reserve(grid->nb_tiles);
    for (unsigned int i = 0; i < grid->nb_tiles; ++i) {
        unsigned int local_idx = grid->offsets[i].idx;
        if (local_idx >= stg->nb_streams) {
            char buf[96];
            std::snprintf(buf, sizeof(buf),
                          "tile %u: offsets.idx %u out of range",
                          i, local_idx);
            err = buf;
            return nullptr;
        }
        AVStream* st = stg->streams[local_idx];
        std::string tile_err;
        AVFramePtr frame = decode_one_frame(fmt_ctx, st->index, tile_err);
        if (!frame) {
            char prefix[96];
            std::snprintf(prefix, sizeof(prefix),
                          "tile %u (stream %d)", i, st->index);
            err = std::string(prefix) + ": " + tile_err;
            return nullptr;
        }
        tile_frames.push_back(std::move(frame));
    }

    // ---- build assembler ----
    HeifTileAssembler assembler;
    std::vector<HeifTileInputDesc> inputs;
    inputs.reserve(grid->nb_tiles);
    for (size_t i = 0; i < tile_frames.size(); ++i) {
        AVFrame* f = tile_frames[i].get();
        HeifTileInputDesc d;
        d.width   = f->width;
        d.height  = f->height;
        d.pix_fmt = static_cast<AVPixelFormat>(f->format);
        if (f->sample_aspect_ratio.num && f->sample_aspect_ratio.den) {
            d.sar = f->sample_aspect_ratio;
        }
        // HEIF stills have no real timebase; xstack only requires that
        // every input shares one, so {1,1} on every src is fine.
        d.time_base = AVRational{1, 1};
        inputs.push_back(d);
    }

    // Pick sink pix_fmt based on caller flags.
    AVPixelFormat sink_av_fmt = AV_PIX_FMT_NONE;
    select_target_pixel_format(
        static_cast<AVPixelFormat>(tile_frames[0]->format), flags, sink_av_fmt);

    if (!assembler.configure(grid, inputs, sink_av_fmt, err)) {
        return nullptr;
    }

    std::vector<AVFrame*> raw_frames;
    raw_frames.reserve(tile_frames.size());
    for (auto& p : tile_frames) raw_frames.push_back(p.get());

    AVFramePtr composed(assembler.assemble(raw_frames, err));
    if (!composed) return nullptr;

    bool has_icc = stream_group_has_icc(stg);

    auto image = std::make_unique<Image>();
    if (!avframe_to_image(composed.get(), *image,
                          src_format, flags, has_icc, err)) {
        return nullptr;
    }
    return image;
}

std::unique_ptr<Image> decode_single_stream(AVFormatContext* fmt_ctx,
                                            int stream_index,
                                            SourceFormat src_format,
                                            std::uint32_t flags,
                                            std::string& err) {
    AVFramePtr frame = decode_one_frame(fmt_ctx, stream_index, err);
    if (!frame) return nullptr;

    bool has_icc = stream_has_icc(fmt_ctx->streams[stream_index]) ||
                   frame_has_icc(frame.get());

    auto image = std::make_unique<Image>();
    if (!avframe_to_image(frame.get(), *image,
                          src_format, flags, has_icc, err)) {
        return nullptr;
    }
    return image;
}

} // namespace

// ============================================================================
// Public entry point
// ============================================================================

std::unique_ptr<Image> load_heif_avif_from_memory(const uint8_t* data,
                                                  std::size_t size,
                                                  SourceFormat fmt,
                                                  std::uint32_t flags,
                                                  std::string& err) {
    err.clear();
    if (!data || size == 0) {
        err = "empty input buffer";
        return nullptr;
    }

    // ---- AVIO: in-memory reader ----
    constexpr int avio_buf_size = 4096;
    auto* avio_buf = static_cast<unsigned char*>(av_malloc(avio_buf_size));
    if (!avio_buf) {
        err = "av_malloc(AVIO buffer) failed";
        return nullptr;
    }

    // The MemReader is owned here and outlives the AVIOContext: the
    // unique_ptr below frees the AVIO context on every return path,
    // and the reader struct is on the stack so it goes away after.
    MemReader reader{data, size, 0};

    AVIOContext* raw_avio = avio_alloc_context(
        avio_buf, avio_buf_size,
        /*write_flag*/ 0,
        &reader,
        &memreader_read,
        /*write_packet*/ nullptr,
        &memreader_seek);
    if (!raw_avio) {
        av_free(avio_buf);
        err = "avio_alloc_context failed";
        return nullptr;
    }
    AVIOContextPtr avio_ctx(raw_avio);

    // ---- AVFormatContext open ----
    AVFormatContext* raw_fmt = avformat_alloc_context();
    if (!raw_fmt) {
        err = "avformat_alloc_context failed";
        return nullptr;
    }
    raw_fmt->pb = avio_ctx.get();
    raw_fmt->flags |= AVFMT_FLAG_CUSTOM_IO;

    // Try the AVIF demuxer first when the caller already knows the
    // format -- saves probing costs and is required for some bare
    // AVIF still images that lack a strong signature.
    const AVInputFormat* hint = nullptr;
    if (fmt == SourceFormat::AVIF) {
        hint = av_find_input_format("avif");
    } else if (fmt == SourceFormat::HEIF) {
        // HEIF lives inside the mov demuxer in current FFmpeg builds.
        hint = av_find_input_format("mov");
    }

    if (int ret = avformat_open_input(&raw_fmt, /*url*/ nullptr, hint, nullptr);
        ret < 0) {
        // avformat_open_input frees raw_fmt on failure; do NOT
        // double-free below.
        set_av_err(err, "avformat_open_input failed", ret);
        return nullptr;
    }
    AVFormatContextPtr fmt_ctx(raw_fmt);

    if (int ret = avformat_find_stream_info(fmt_ctx.get(), nullptr); ret < 0) {
        set_av_err(err, "avformat_find_stream_info failed", ret);
        return nullptr;
    }

    // ---- pick the main image entry ----
    if (const AVStreamGroup* stg = find_tile_grid_group(fmt_ctx.get())) {
        const AVStreamGroupTileGrid* grid = stg->params.tile_grid;

        // Single-tile grids are equivalent to a primary item with no
        // composition needed.  Take the fast path.
        if (grid && grid->nb_tiles == 1 && stg->nb_streams >= 1) {
            int stream_idx = stg->streams[0]->index;
            return decode_single_stream(fmt_ctx.get(), stream_idx,
                                        fmt, flags, err);
        }

        return decode_tile_grid(fmt_ctx.get(), stg, fmt, flags, err);
    }

    // Fallback: no tile-grid group, so pick the best video stream and
    // decode its primary frame.
    int best = av_find_best_stream(fmt_ctx.get(), AVMEDIA_TYPE_VIDEO,
                                   -1, -1, nullptr, 0);
    if (best < 0) {
        set_av_err(err, "av_find_best_stream failed", best);
        return nullptr;
    }
    return decode_single_stream(fmt_ctx.get(), best, fmt, flags, err);
}

} // namespace idiff

#endif // IDIFF_HAVE_FFMPEG_IMAGE_DECODE
