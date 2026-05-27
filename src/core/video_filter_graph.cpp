// SPDX-License-Identifier: BSD-2-Clause
#ifdef IDIFF_HAVE_FFMPEG

#include "core/video_filter_graph.h"
#include "util/logger.h"

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

#include <cstdio>
#include <cstring>

namespace idiff {

namespace {

// Format an AVRational as "num/den" into a fixed-size buffer.  Returns
// the number of characters written (excluding NUL).
int format_rational(char* buf, size_t cap, AVRational r) {
    if (r.num == 0 || r.den == 0) {
        return std::snprintf(buf, cap, "1/1");
    }
    return std::snprintf(buf, cap, "%d/%d", r.num, r.den);
}

const char* pix_fmt_name(AVPixelFormat fmt) {
    const char* n = av_get_pix_fmt_name(fmt);
    return n ? n : "?";
}

} // namespace

// ============================================================================
// Impl
// ============================================================================

struct VideoFilterGraph::Impl {
    AVFilterGraph* graph = nullptr;
    AVFilterContext* src_ctx = nullptr;
    AVFilterContext* sink_ctx = nullptr;

    VideoFilterInputParams in_params;
    VideoFilterOutputParams out_params;

    bool configured = false;

    void close() noexcept {
        if (graph) {
            avfilter_graph_free(&graph);  // also frees src_ctx / sink_ctx
        }
        src_ctx = nullptr;
        sink_ctx = nullptr;
        configured = false;
    }
};

// ============================================================================
// VideoFilterGraph
// ============================================================================

VideoFilterGraph::VideoFilterGraph() : impl_(std::make_unique<Impl>()) {}

VideoFilterGraph::~VideoFilterGraph() {
    if (impl_) impl_->close();
}

bool VideoFilterGraph::is_configured() const noexcept {
    return impl_ && impl_->configured;
}

const VideoFilterInputParams& VideoFilterGraph::input_params() const noexcept {
    return impl_->in_params;
}

const VideoFilterOutputParams& VideoFilterGraph::output_params() const noexcept {
    return impl_->out_params;
}

void VideoFilterGraph::reset() noexcept {
    if (impl_) impl_->close();
}

bool VideoFilterGraph::configure(const VideoFilterInputParams& in,
                                 const VideoFilterOutputParams& out,
                                 std::string& err) {
    impl_->close();

    if (in.width <= 0 || in.height <= 0 || in.pix_fmt == AV_PIX_FMT_NONE) {
        err = "invalid input params";
        return false;
    }
    if (out.pix_fmt == AV_PIX_FMT_NONE) {
        err = "invalid output pixel format";
        return false;
    }

    impl_->graph = avfilter_graph_alloc();
    if (!impl_->graph) {
        err = "avfilter_graph_alloc failed";
        return false;
    }

    // ---- buffer source ----
    //
    // The buffer src needs the source frame's geometry, format, SAR
    // and time_base.  Colour metadata is set on the frames themselves
    // (and replicated as vf_scale's in_* options below).

    const AVFilter* src_filter = avfilter_get_by_name("buffer");
    const AVFilter* sink_filter = avfilter_get_by_name("buffersink");
    if (!src_filter || !sink_filter) {
        err = "buffer/buffersink filter not registered";
        impl_->close();
        return false;
    }

    char src_args[256];
    char sar_buf[32];
    char tb_buf[32];
    format_rational(sar_buf, sizeof(sar_buf), in.sar);
    format_rational(tb_buf, sizeof(tb_buf), in.time_base);
    std::snprintf(src_args, sizeof(src_args),
                  "video_size=%dx%d:pix_fmt=%d:time_base=%s:pixel_aspect=%s",
                  in.width, in.height,
                  static_cast<int>(in.pix_fmt),
                  tb_buf, sar_buf);

    int ret = avfilter_graph_create_filter(&impl_->src_ctx, src_filter,
                                           "in", src_args, nullptr,
                                           impl_->graph);
    if (ret < 0) {
        char e[128];
        av_strerror(ret, e, sizeof(e));
        err = std::string("buffer src create failed: ") + e;
        impl_->close();
        return false;
    }

    // ---- buffer sink ----

    ret = avfilter_graph_create_filter(&impl_->sink_ctx, sink_filter,
                                       "out", nullptr, nullptr,
                                       impl_->graph);
    if (ret < 0) {
        char e[128];
        av_strerror(ret, e, sizeof(e));
        err = std::string("buffer sink create failed: ") + e;
        impl_->close();
        return false;
    }

    // Constrain the sink to the desired pixel format -- vf_scale will
    // negotiate to match this.  We use av_opt_set_bin() directly to
    // avoid the av_opt_set_int_list() macro, which expands into a
    // deprecated helper on FFmpeg 8 headers.
    const AVPixelFormat sink_pix_fmts[] = {out.pix_fmt, AV_PIX_FMT_NONE};
    ret = av_opt_set_bin(impl_->sink_ctx, "pix_fmts",
                         reinterpret_cast<const uint8_t*>(sink_pix_fmts),
                         sizeof(AVPixelFormat),
                         AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) {
        err = "buffer sink pix_fmts set failed";
        impl_->close();
        return false;
    }

    // ---- scale filter ----
    //
    // We construct it as a free-standing AVFilterContext (rather than
    // through avfilter_graph_parse_ptr) so all colour parameters can
    // be set via the option API -- safer than building a filter
    // description string and dealing with quoting.

    const AVFilter* scale_filter = avfilter_get_by_name("scale");
    if (!scale_filter) {
        err = "scale filter not registered";
        impl_->close();
        return false;
    }

    AVFilterContext* scale_ctx = nullptr;
    ret = avfilter_graph_create_filter(&scale_ctx, scale_filter,
                                       "scale", nullptr, nullptr,
                                       impl_->graph);
    if (ret < 0) {
        char e[128];
        av_strerror(ret, e, sizeof(e));
        err = std::string("scale create failed: ") + e;
        impl_->close();
        return false;
    }

    // Output dimensions: 0 means "keep source size" -- pass through as
    // the FFmpeg conventional iw/ih sentinels by using -1 expressions
    // or simply reusing the source size.
    const int out_w = (out.width  > 0) ? out.width  : in.width;
    const int out_h = (out.height > 0) ? out.height : in.height;

    auto set_int = [&](const char* name, int64_t v) -> int {
        return av_opt_set_int(scale_ctx, name, v, AV_OPT_SEARCH_CHILDREN);
    };
    auto set_str = [&](const char* name, const char* v) -> int {
        return av_opt_set(scale_ctx, name, v, AV_OPT_SEARCH_CHILDREN);
    };

    char wbuf[16], hbuf[16];
    std::snprintf(wbuf, sizeof(wbuf), "%d", out_w);
    std::snprintf(hbuf, sizeof(hbuf), "%d", out_h);

    int rc = 0;
    rc |= set_str("w", wbuf);
    rc |= set_str("h", hbuf);
    rc |= set_str("flags", "bilinear");

    // Source colour properties.
    rc |= set_int("in_range",      in.range);
    rc |= set_int("in_color_matrix",   in.matrix);
    rc |= set_int("in_primaries",  in.primaries);
    rc |= set_int("in_transfer",   in.transfer);

    // Destination colour properties.
    rc |= set_int("out_range",     out.range);
    rc |= set_int("out_color_matrix",  out.matrix);
    rc |= set_int("out_primaries", out.primaries);
    rc |= set_int("out_transfer",  out.transfer);

    if (rc != 0) {
        // FFmpeg >= 8.1 is mandatory; vf_scale must accept every
        // colour option we set.  A failure here means the build is
        // mis-linked or someone replaced libswscale -- fail loudly.
        err = "vf_scale rejected one or more colour options";
        impl_->close();
        return false;
    }

    // ---- link buffer -> scale -> buffersink ----

    ret = avfilter_link(impl_->src_ctx, 0, scale_ctx, 0);
    if (ret < 0) {
        err = "link src->scale failed";
        impl_->close();
        return false;
    }
    ret = avfilter_link(scale_ctx, 0, impl_->sink_ctx, 0);
    if (ret < 0) {
        err = "link scale->sink failed";
        impl_->close();
        return false;
    }

    ret = avfilter_graph_config(impl_->graph, nullptr);
    if (ret < 0) {
        char e[128];
        av_strerror(ret, e, sizeof(e));
        err = std::string("graph config failed: ") + e;
        impl_->close();
        return false;
    }

    impl_->in_params = in;
    impl_->out_params = out;
    impl_->configured = true;

    LOG_INFO("VideoFilterGraph: configured "
             "%dx%d %s -> %dx%d %s "
             "(in: range=%d matrix=%d prim=%d trc=%d; "
             "out: range=%d matrix=%d prim=%d trc=%d)",
             in.width, in.height, pix_fmt_name(in.pix_fmt),
             out_w, out_h, pix_fmt_name(out.pix_fmt),
             static_cast<int>(in.range),  static_cast<int>(in.matrix),
             static_cast<int>(in.primaries), static_cast<int>(in.transfer),
             static_cast<int>(out.range), static_cast<int>(out.matrix),
             static_cast<int>(out.primaries), static_cast<int>(out.transfer));

    return true;
}

bool VideoFilterGraph::needs_reconfigure(const AVFrame* in) const noexcept {
    if (!is_configured() || !in) return true;
    const VideoFilterInputParams& p = impl_->in_params;
    if (in->width != p.width || in->height != p.height) return true;
    if (static_cast<AVPixelFormat>(in->format) != p.pix_fmt) return true;
    if (in->sample_aspect_ratio.num != p.sar.num ||
        in->sample_aspect_ratio.den != p.sar.den) {
        // Treat 0/1 (unset) as compatible with the configured SAR --
        // many decoders simply leave SAR unset on individual frames.
        if (!(in->sample_aspect_ratio.num == 0 &&
              in->sample_aspect_ratio.den == 1)) {
            return true;
        }
    }
    if (in->color_range != p.range) return true;
    if (in->colorspace  != p.matrix) return true;
    if (in->color_primaries != p.primaries) return true;
    if (in->color_trc != p.transfer) return true;
    return false;
}

AVFrame* VideoFilterGraph::process(const AVFrame* in, std::string& err) {
    if (!is_configured()) {
        err = "graph not configured";
        return nullptr;
    }
    if (!in) {
        err = "null input frame";
        return nullptr;
    }

    int ret = av_buffersrc_add_frame_flags(
        impl_->src_ctx, const_cast<AVFrame*>(in),
        AV_BUFFERSRC_FLAG_KEEP_REF);
    if (ret < 0) {
        char e[128];
        av_strerror(ret, e, sizeof(e));
        err = std::string("buffersrc add_frame failed: ") + e;
        return nullptr;
    }

    AVFrame* out = av_frame_alloc();
    if (!out) {
        err = "av_frame_alloc failed";
        return nullptr;
    }

    ret = av_buffersink_get_frame(impl_->sink_ctx, out);
    if (ret < 0) {
        char e[128];
        av_strerror(ret, e, sizeof(e));
        err = std::string("buffersink get_frame failed: ") + e;
        av_frame_free(&out);
        return nullptr;
    }
    return out;
}

} // namespace idiff

#endif // IDIFF_HAVE_FFMPEG
