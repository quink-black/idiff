// SPDX-License-Identifier: BSD-2-Clause
#ifdef IDIFF_HAVE_FFMPEG_IMAGE_DECODE

#include "core/detail/heif_tile_assembler.h"

#include "util/logger.h"

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace idiff {

namespace {

void set_av_err(std::string& err, const char* prefix, int ret) {
    char buf[128];
    av_strerror(ret, buf, sizeof(buf));
    err = std::string(prefix) + ": " + buf;
}

const char* pix_fmt_name(AVPixelFormat fmt) {
    const char* n = av_get_pix_fmt_name(fmt);
    return n ? n : "?";
}

} // namespace

struct HeifTileAssembler::Impl {
    AVFilterGraph* graph = nullptr;
    std::vector<AVFilterContext*> src_ctxs;  // one per tile, same order as grid->offsets
    AVFilterContext* sink_ctx = nullptr;

    bool configured = false;

    // Logged once per assemble() so debug-level output traces every
    // composition, while info-level logs stay quiet.  Saved at
    // configure() time because the underlying grid pointer is owned
    // by the AVFormatContext and may be freed before assemble().
    int nb_tiles = 0;
    int coded_w = 0, coded_h = 0;
    int out_w = 0, out_h = 0;

    void close() noexcept {
        if (graph) {
            avfilter_graph_free(&graph);  // also frees src/sink ctxs
        }
        src_ctxs.clear();
        sink_ctx = nullptr;
        configured = false;
    }
};

HeifTileAssembler::HeifTileAssembler() : impl_(std::make_unique<Impl>()) {}

HeifTileAssembler::~HeifTileAssembler() {
    if (impl_) impl_->close();
}

bool HeifTileAssembler::is_configured() const noexcept {
    return impl_ && impl_->configured;
}

void HeifTileAssembler::reset() noexcept {
    if (impl_) impl_->close();
}

bool HeifTileAssembler::configure(const AVStreamGroupTileGrid* grid,
                                  const std::vector<HeifTileInputDesc>& inputs,
                                  AVPixelFormat out_pix_fmt,
                                  std::string& err) {
    impl_->close();

    if (!grid) {
        err = "null tile grid";
        return false;
    }
    if (grid->nb_tiles == 0) {
        err = "tile grid has zero tiles";
        return false;
    }
    if (inputs.size() != grid->nb_tiles) {
        char buf[96];
        std::snprintf(buf, sizeof(buf),
                      "input descriptor count %zu != nb_tiles %u",
                      inputs.size(),
                      static_cast<unsigned>(grid->nb_tiles));
        err = buf;
        return false;
    }
    if (out_pix_fmt == AV_PIX_FMT_NONE) {
        err = "invalid output pixel format";
        return false;
    }

    impl_->graph = avfilter_graph_alloc();
    if (!impl_->graph) {
        err = "avfilter_graph_alloc failed";
        return false;
    }

    const AVFilter* buffer_filter   = avfilter_get_by_name("buffer");
    const AVFilter* xstack_filter   = avfilter_get_by_name("xstack");
    const AVFilter* crop_filter     = avfilter_get_by_name("crop");
    const AVFilter* buffersink_flt  = avfilter_get_by_name("buffersink");
    if (!buffer_filter || !xstack_filter || !crop_filter || !buffersink_flt) {
        err = "buffer/xstack/crop/buffersink filter not registered";
        impl_->close();
        return false;
    }

    // ---- N buffer sources -----------------------------------------------
    impl_->src_ctxs.reserve(grid->nb_tiles);
    for (unsigned int i = 0; i < grid->nb_tiles; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "tile_in_%u", i);
        AVFilterContext* src = avfilter_graph_alloc_filter(
            impl_->graph, buffer_filter, name);
        if (!src) {
            err = "buffer src alloc failed";
            impl_->close();
            return false;
        }

        const HeifTileInputDesc& d = inputs[i];
        if (d.width <= 0 || d.height <= 0 || d.pix_fmt == AV_PIX_FMT_NONE) {
            char buf[96];
            std::snprintf(buf, sizeof(buf),
                          "tile %u: invalid input descriptor", i);
            err = buf;
            impl_->close();
            return false;
        }

        const AVRational tb = (d.time_base.num && d.time_base.den)
            ? d.time_base : AVRational{1, 1};
        const AVRational sar = (d.sar.num && d.sar.den)
            ? d.sar : AVRational{1, 1};

        int rc = 0;
        rc |= av_opt_set_int(src, "width",  d.width,
                             AV_OPT_SEARCH_CHILDREN);
        rc |= av_opt_set_int(src, "height", d.height,
                             AV_OPT_SEARCH_CHILDREN);
        rc |= av_opt_set_int(src, "pix_fmt", static_cast<int>(d.pix_fmt),
                             AV_OPT_SEARCH_CHILDREN);
        rc |= av_opt_set_q(src, "time_base", tb, AV_OPT_SEARCH_CHILDREN);
        rc |= av_opt_set_q(src, "pixel_aspect", sar,
                           AV_OPT_SEARCH_CHILDREN);
        if (rc != 0) {
            err = "buffer src option set failed";
            impl_->close();
            return false;
        }

        if (int ret = avfilter_init_str(src, nullptr); ret < 0) {
            set_av_err(err, "buffer src init failed", ret);
            impl_->close();
            return false;
        }
        impl_->src_ctxs.push_back(src);
    }

    // ---- xstack ----------------------------------------------------------
    //
    // layout = "x0_y0|x1_y1|...|x_{N-1}_y_{N-1}"  -- absolute pixel
    // offsets per input.  fill = "0xRRGGBB@0xAA" gives xstack the
    // canvas background colour (matches grid->background[]).
    //
    // We let xstack's `inputs=N` produce exactly N input pads; the
    // canvas size is implied by max(x_i + w_i, y_i + h_i) but we
    // crop to (coded_w x coded_h) below to handle the (rare) case
    // where the encoded canvas is taller than the union of tiles
    // (that gap is what `background` is intended to fill).

    AVFilterContext* xstack_ctx = avfilter_graph_alloc_filter(
        impl_->graph, xstack_filter, "xstack");
    if (!xstack_ctx) {
        err = "xstack alloc failed";
        impl_->close();
        return false;
    }

    std::string layout;
    layout.reserve(grid->nb_tiles * 16);
    for (unsigned int i = 0; i < grid->nb_tiles; ++i) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%d_%d",
                      grid->offsets[i].horizontal,
                      grid->offsets[i].vertical);
        if (i) layout += '|';
        layout += buf;
    }

    char fill[32];
    std::snprintf(fill, sizeof(fill), "0x%02X%02X%02X@0x%02X",
                  grid->background[0], grid->background[1],
                  grid->background[2], grid->background[3]);

    {
        int rc = 0;
        rc |= av_opt_set_int(xstack_ctx, "inputs", grid->nb_tiles,
                             AV_OPT_SEARCH_CHILDREN);
        rc |= av_opt_set(xstack_ctx, "layout", layout.c_str(),
                         AV_OPT_SEARCH_CHILDREN);
        rc |= av_opt_set(xstack_ctx, "fill", fill,
                         AV_OPT_SEARCH_CHILDREN);
        if (rc != 0) {
            err = "xstack rejected one or more options";
            impl_->close();
            return false;
        }
    }

    if (int ret = avfilter_init_str(xstack_ctx, nullptr); ret < 0) {
        set_av_err(err, "xstack init failed", ret);
        impl_->close();
        return false;
    }

    // ---- crop ------------------------------------------------------------
    //
    // Trim xstack's coded canvas down to grid->{width,height} starting
    // at (horizontal_offset, vertical_offset).  We always insert the
    // crop -- when no crop is required, configure() should set
    // those parameters to a no-op (full canvas), but the assembler
    // does not assume that.

    AVFilterContext* crop_ctx = avfilter_graph_alloc_filter(
        impl_->graph, crop_filter, "crop");
    if (!crop_ctx) {
        err = "crop alloc failed";
        impl_->close();
        return false;
    }

    {
        char wbuf[16], hbuf[16], xbuf[16], ybuf[16];
        // Resolve to coded canvas if width/height are zero, which can
        // happen on malformed inputs.
        const int w = (grid->width  > 0) ? grid->width  : grid->coded_width;
        const int h = (grid->height > 0) ? grid->height : grid->coded_height;
        std::snprintf(wbuf, sizeof(wbuf), "%d", w);
        std::snprintf(hbuf, sizeof(hbuf), "%d", h);
        std::snprintf(xbuf, sizeof(xbuf), "%d", grid->horizontal_offset);
        std::snprintf(ybuf, sizeof(ybuf), "%d", grid->vertical_offset);

        int rc = 0;
        rc |= av_opt_set(crop_ctx, "w", wbuf, AV_OPT_SEARCH_CHILDREN);
        rc |= av_opt_set(crop_ctx, "h", hbuf, AV_OPT_SEARCH_CHILDREN);
        rc |= av_opt_set(crop_ctx, "x", xbuf, AV_OPT_SEARCH_CHILDREN);
        rc |= av_opt_set(crop_ctx, "y", ybuf, AV_OPT_SEARCH_CHILDREN);
        if (rc != 0) {
            err = "crop rejected one or more options";
            impl_->close();
            return false;
        }

        impl_->out_w = w;
        impl_->out_h = h;
    }

    if (int ret = avfilter_init_str(crop_ctx, nullptr); ret < 0) {
        set_av_err(err, "crop init failed", ret);
        impl_->close();
        return false;
    }

    // ---- buffersink ------------------------------------------------------
    //
    // Constrain the sink to a single packed pixel format.  libavfilter
    // will auto-insert a `format` (and, if needed, `scale`) node ahead
    // of the sink to satisfy the constraint -- exactly what we want
    // for "give me RGB24 / RGBA / RGB48LE / RGBA64LE no matter what
    // the source YUV layout was".

    impl_->sink_ctx = avfilter_graph_alloc_filter(
        impl_->graph, buffersink_flt, "out");
    if (!impl_->sink_ctx) {
        err = "buffer sink alloc failed";
        impl_->close();
        return false;
    }

    {
        const AVPixelFormat sink_pix_fmts[] = {out_pix_fmt};
        if (int ret = av_opt_set_array(
                impl_->sink_ctx, "pixel_formats",
                AV_OPT_SEARCH_CHILDREN,
                0, 1, AV_OPT_TYPE_PIXEL_FMT, sink_pix_fmts);
            ret < 0) {
            set_av_err(err, "buffer sink pixel_formats set failed", ret);
            impl_->close();
            return false;
        }
    }

    if (int ret = avfilter_init_str(impl_->sink_ctx, nullptr); ret < 0) {
        set_av_err(err, "buffer sink init failed", ret);
        impl_->close();
        return false;
    }

    // ---- linking ---------------------------------------------------------
    //
    // src_i -> xstack:i, xstack:0 -> crop:0 -> sink:0.

    for (unsigned int i = 0; i < grid->nb_tiles; ++i) {
        if (int ret = avfilter_link(impl_->src_ctxs[i], 0,
                                    xstack_ctx, i);
            ret < 0) {
            char prefix[64];
            std::snprintf(prefix, sizeof(prefix),
                          "link tile %u -> xstack failed", i);
            set_av_err(err, prefix, ret);
            impl_->close();
            return false;
        }
    }
    if (int ret = avfilter_link(xstack_ctx, 0, crop_ctx, 0); ret < 0) {
        set_av_err(err, "link xstack -> crop failed", ret);
        impl_->close();
        return false;
    }
    if (int ret = avfilter_link(crop_ctx, 0, impl_->sink_ctx, 0); ret < 0) {
        set_av_err(err, "link crop -> sink failed", ret);
        impl_->close();
        return false;
    }

    if (int ret = avfilter_graph_config(impl_->graph, nullptr); ret < 0) {
        set_av_err(err, "graph config failed", ret);
        impl_->close();
        return false;
    }

    impl_->nb_tiles = static_cast<int>(grid->nb_tiles);
    impl_->coded_w  = grid->coded_width;
    impl_->coded_h  = grid->coded_height;
    impl_->configured = true;

    LOG_INFO("HeifTileAssembler: configured %d tiles, "
             "canvas %dx%d -> cropped %dx%d, sink=%s",
             impl_->nb_tiles, impl_->coded_w, impl_->coded_h,
             impl_->out_w, impl_->out_h,
             pix_fmt_name(out_pix_fmt));
    return true;
}

AVFrame* HeifTileAssembler::assemble(const std::vector<AVFrame*>& tiles,
                                     std::string& err) {
    if (!is_configured()) {
        err = "assembler not configured";
        return nullptr;
    }
    if (tiles.size() != impl_->src_ctxs.size()) {
        char buf[96];
        std::snprintf(buf, sizeof(buf),
                      "tile frame count %zu != configured inputs %zu",
                      tiles.size(), impl_->src_ctxs.size());
        err = buf;
        return nullptr;
    }

    // Push every tile.  KEEP_REF so the caller's frames are not
    // consumed; the graph holds its own refs internally.
    for (size_t i = 0; i < tiles.size(); ++i) {
        AVFrame* f = tiles[i];
        if (!f) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "tile %zu: null frame", i);
            err = buf;
            return nullptr;
        }
        if (int ret = av_buffersrc_add_frame_flags(
                impl_->src_ctxs[i], f, AV_BUFFERSRC_FLAG_KEEP_REF);
            ret < 0) {
            char prefix[64];
            std::snprintf(prefix, sizeof(prefix),
                          "tile %zu: buffersrc add_frame failed", i);
            set_av_err(err, prefix, ret);
            return nullptr;
        }
    }

    // Pull the single composed output frame.  xstack waits until it
    // has one frame on every input pad, so a single get_frame call is
    // enough; we do not need to drain in a loop.
    AVFrame* out = av_frame_alloc();
    if (!out) {
        err = "av_frame_alloc failed";
        return nullptr;
    }
    if (int ret = av_buffersink_get_frame(impl_->sink_ctx, out); ret < 0) {
        set_av_err(err, "buffersink get_frame failed", ret);
        av_frame_free(&out);
        return nullptr;
    }

    LOG_DEBUG("HeifTileAssembler: assembled %d tiles "
              "(canvas %dx%d -> output %dx%d)",
              impl_->nb_tiles, impl_->coded_w, impl_->coded_h,
              out->width, out->height);
    return out;
}

} // namespace idiff

#endif // IDIFF_HAVE_FFMPEG_IMAGE_DECODE
