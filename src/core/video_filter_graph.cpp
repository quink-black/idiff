// SPDX-License-Identifier: BSD-2-Clause
#ifdef IDIFF_HAVE_FFMPEG

#include "core/video_filter_graph.h"
#include "util/logger.h"

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/frame.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

#include <cstdio>
#include <cstring>

namespace idiff {

namespace {

const char* pix_fmt_name(AVPixelFormat fmt) {
    const char* n = av_get_pix_fmt_name(fmt);
    return n ? n : "?";
}

// Format an FFmpeg negative return code into the caller's `err`
// string with a leading prefix.  Centralises the boilerplate so
// every error path reads the same way.
void set_av_err(std::string& err, const char* prefix, int ret) {
    char buf[128];
    av_strerror(ret, buf, sizeof(buf));
    err = std::string(prefix) + ": " + buf;
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

    // True when the configured pipeline is HLG source -> non-HDR sink.
    // process() then injects a 203-nit mastering display side data
    // onto a clone of the caller's frame so swscale's HLG OOTF
    // collapses to ~1.0 and tone mapping is effectively disabled.
    // See video_filter_graph.h for the full rationale.
    bool need_hlg_sdr_fix = false;

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

std::string VideoFilterGraph::graph_description() const {
    if (!is_configured() || !impl_->graph) return {};
    char* dump = avfilter_graph_dump(impl_->graph, nullptr);
    if (!dump) return {};
    std::string s(dump);
    av_free(dump);
    return s;
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

    const AVFilter* src_filter   = avfilter_get_by_name("buffer");
    const AVFilter* sink_filter  = avfilter_get_by_name("buffersink");
    const AVFilter* scale_filter = avfilter_get_by_name("scale");
    if (!src_filter || !sink_filter || !scale_filter) {
        err = "buffer/buffersink/scale filter not registered";
        impl_->close();
        return false;
    }

    // ---- buffer source ----------------------------------------------------
    //
    // Every option below is init-time on vf_buffer.  Allocate the
    // context, set options via av_opt_*, then init explicitly.  Using
    // avfilter_graph_create_filter() would init immediately and any
    // subsequent option set would fail with "not a runtime option".

    impl_->src_ctx = avfilter_graph_alloc_filter(
        impl_->graph, src_filter, "in");
    if (!impl_->src_ctx) {
        err = "buffer src alloc failed";
        impl_->close();
        return false;
    }

    const AVRational tb =
        (in.time_base.num && in.time_base.den) ? in.time_base
                                               : AVRational{1, 1};
    const AVRational sar =
        (in.sar.num && in.sar.den) ? in.sar : AVRational{1, 1};

    int rc = 0;
    rc |= av_opt_set_int(impl_->src_ctx, "width",  in.width,
                         AV_OPT_SEARCH_CHILDREN);
    rc |= av_opt_set_int(impl_->src_ctx, "height", in.height,
                         AV_OPT_SEARCH_CHILDREN);
    rc |= av_opt_set_int(impl_->src_ctx, "pix_fmt",
                         static_cast<int>(in.pix_fmt),
                         AV_OPT_SEARCH_CHILDREN);
    rc |= av_opt_set_q(impl_->src_ctx, "time_base",    tb,
                       AV_OPT_SEARCH_CHILDREN);
    rc |= av_opt_set_q(impl_->src_ctx, "pixel_aspect", sar,
                       AV_OPT_SEARCH_CHILDREN);
    rc |= av_opt_set_int(impl_->src_ctx, "colorspace", in.matrix,
                         AV_OPT_SEARCH_CHILDREN);
    rc |= av_opt_set_int(impl_->src_ctx, "range",      in.range,
                         AV_OPT_SEARCH_CHILDREN);
    if (rc != 0) {
        err = "buffer src option set failed";
        impl_->close();
        return false;
    }

    if (int ret = avfilter_init_str(impl_->src_ctx, nullptr); ret < 0) {
        set_av_err(err, "buffer src init failed", ret);
        impl_->close();
        return false;
    }

    // ---- buffer sink ------------------------------------------------------
    //
    // `pixel_formats` is the modern array-typed init-time option that
    // replaces the deprecated `pix_fmts` blob.  Constraining the sink
    // to a single format means we never have to insert an explicit
    // `format` filter ourselves: libavfilter inserts an auto-format
    // node only if scale's output does not already match.

    impl_->sink_ctx = avfilter_graph_alloc_filter(
        impl_->graph, sink_filter, "out");
    if (!impl_->sink_ctx) {
        err = "buffer sink alloc failed";
        impl_->close();
        return false;
    }

    const AVPixelFormat sink_pix_fmts[] = {out.pix_fmt};
    if (int ret = av_opt_set_array(
            impl_->sink_ctx, "pixel_formats", AV_OPT_SEARCH_CHILDREN,
            0, 1, AV_OPT_TYPE_PIXEL_FMT, sink_pix_fmts);
        ret < 0) {
        set_av_err(err, "buffer sink pixel_formats set failed", ret);
        impl_->close();
        return false;
    }

    if (int ret = avfilter_init_str(impl_->sink_ctx, nullptr); ret < 0) {
        set_av_err(err, "buffer sink init failed", ret);
        impl_->close();
        return false;
    }

    // ---- scale filter -----------------------------------------------------
    //
    // Built free-standing so colour parameters go through the option
    // API rather than a quoted description string.
    //
    // SDR vs HDR semantic split:
    //
    //   * SDR sources -- we set ONLY in_color_matrix + in_range.
    //     Without primaries / transfer, vf_scale does a pure YUV->RGB
    //     matrix decode and treats the source RGB primaries as the
    //     destination's, i.e. no gamut or gamma conversion.  This is
    //     the "as encoded" semantics every consumer player implements
    //     for SDR display: if a clip is tagged SMPTE-170M, decoding
    //     it through the BT.601 matrix should still display pure red
    //     for an encoded pure red, not (245, 41, 0) -- the latter is
    //     what a perceptually-correct gamut conversion to BT.709
    //     produces, and is *not* what users expect from a video
    //     viewer.
    //
    //   * HDR sources (PQ / HLG transfer) -- we set the full four-
    //     tuple on both ends.  vf_scale then performs gamma-correct
    //     linear-light conversion plus BT.2020 -> BT.709 gamut remap,
    //     which is the only correct way to display HDR content on an
    //     SDR sink.
    //
    // This is a real semantic split, not defensive code: the SDR
    // tests in test_video_filter_graph.cpp lock the behaviour in
    // place -- if anyone tries to "simplify" by always passing the
    // full four-tuple, BT.601 red will land at (245, 41, 0) and the
    // tests will fail immediately.
    //
    // Likewise we deliberately do NOT set `flags`.  Forcing
    // `flags=bilinear` disables libswscale's full-precision colour
    // pipeline (it picks a fast integer path), which silently breaks
    // HDR conversions even when the colour options below say
    // otherwise.

    AVFilterContext* scale_ctx = avfilter_graph_alloc_filter(
        impl_->graph, scale_filter, "scale");
    if (!scale_ctx) {
        err = "scale alloc failed";
        impl_->close();
        return false;
    }

    const int out_w = (out.width  > 0) ? out.width  : in.width;
    const int out_h = (out.height > 0) ? out.height : in.height;
    char wbuf[16], hbuf[16];
    std::snprintf(wbuf, sizeof(wbuf), "%d", out_w);
    std::snprintf(hbuf, sizeof(hbuf), "%d", out_h);

    const bool src_is_hdr = (in.transfer == AVCOL_TRC_SMPTE2084 ||
                             in.transfer == AVCOL_TRC_ARIB_STD_B67);

    // For RGB sinks the destination matrix is implicit in the pixel
    // format -- explicitly setting out_color_matrix=RGB on packed
    // RGB outputs makes vf_scale apply an unwanted second matrix
    // step.  For YUV / gray destinations we do need to tell vf_scale
    // which matrix to encode into.  Detect from the pixel descriptor:
    // RGB colour model (AV_PIX_FMT_FLAG_RGB) and packed layout (PLANAR
    // not set).  FLAG_RGB alone is not enough -- GBRP / GBRAP are
    // planar RGB and must not be treated as a packed-RGB sink.  This
    // covers every packed RGB sink the loader reuses (RGB24, RGBA,
    // RGB48, RGBA64) without enumerating formats by hand.
    const AVPixFmtDescriptor* out_desc = av_pix_fmt_desc_get(out.pix_fmt);
    const bool out_is_packed_rgb =
        out_desc &&
        (out_desc->flags & AV_PIX_FMT_FLAG_RGB) &&
        !(out_desc->flags & AV_PIX_FMT_FLAG_PLANAR);

    rc = 0;
    rc |= av_opt_set    (scale_ctx, "w", wbuf, AV_OPT_SEARCH_CHILDREN);
    rc |= av_opt_set    (scale_ctx, "h", hbuf, AV_OPT_SEARCH_CHILDREN);
    rc |= av_opt_set_int(scale_ctx, "in_range",        in.range,
                         AV_OPT_SEARCH_CHILDREN);
    rc |= av_opt_set_int(scale_ctx, "in_color_matrix", in.matrix,
                         AV_OPT_SEARCH_CHILDREN);
    if (src_is_hdr) {
        rc |= av_opt_set_int(scale_ctx, "in_primaries", in.primaries,
                             AV_OPT_SEARCH_CHILDREN);
        rc |= av_opt_set_int(scale_ctx, "in_transfer",  in.transfer,
                             AV_OPT_SEARCH_CHILDREN);
    }
    rc |= av_opt_set_int(scale_ctx, "out_range", out.range,
                         AV_OPT_SEARCH_CHILDREN);
    if (!out_is_packed_rgb) {
        rc |= av_opt_set_int(scale_ctx, "out_color_matrix", out.matrix,
                             AV_OPT_SEARCH_CHILDREN);
    }
    if (src_is_hdr) {
        rc |= av_opt_set_int(scale_ctx, "out_primaries", out.primaries,
                             AV_OPT_SEARCH_CHILDREN);
        rc |= av_opt_set_int(scale_ctx, "out_transfer",  out.transfer,
                             AV_OPT_SEARCH_CHILDREN);
    }
    if (rc != 0) {
        err = "vf_scale rejected one or more colour options";
        impl_->close();
        return false;
    }

    if (int ret = avfilter_init_str(scale_ctx, nullptr); ret < 0) {
        set_av_err(err, "scale init failed", ret);
        impl_->close();
        return false;
    }

    // ---- link buffer -> scale -> buffersink -------------------------------

    if (int ret = avfilter_link(impl_->src_ctx, 0, scale_ctx, 0); ret < 0) {
        set_av_err(err, "link src->scale failed", ret);
        impl_->close();
        return false;
    }
    if (int ret = avfilter_link(scale_ctx, 0, impl_->sink_ctx, 0); ret < 0) {
        set_av_err(err, "link scale->sink failed", ret);
        impl_->close();
        return false;
    }

    if (int ret = avfilter_graph_config(impl_->graph, nullptr); ret < 0) {
        set_av_err(err, "graph config failed", ret);
        impl_->close();
        return false;
    }

    // Decide once at configure time whether process() needs to
    // inject the SDR mastering display.  Mirrors codec's IsHlgToSdr:
    // only when src is HLG (ARIB-STD-B67) and dst is an explicitly
    // non-HDR transfer (i.e. neither HLG nor PQ).  Anything else --
    // including HLG->HLG passthrough -- leaves swscale to its
    // default behaviour.
    impl_->need_hlg_sdr_fix =
        in.transfer == AVCOL_TRC_ARIB_STD_B67 &&
        out.transfer != AVCOL_TRC_UNSPECIFIED &&
        out.transfer != AVCOL_TRC_ARIB_STD_B67 &&
        out.transfer != AVCOL_TRC_SMPTE2084;

    impl_->in_params  = in;
    impl_->out_params = out;
    impl_->configured = true;

    LOG_INFO("VideoFilterGraph: configured "
             "%dx%d %s -> %dx%d %s "
             "(in: range=%d matrix=%d prim=%d trc=%d; "
             "out: range=%d matrix=%d prim=%d trc=%d)",
             in.width, in.height, pix_fmt_name(in.pix_fmt),
             out_w, out_h, pix_fmt_name(out.pix_fmt),
             static_cast<int>(in.range),     static_cast<int>(in.matrix),
             static_cast<int>(in.primaries), static_cast<int>(in.transfer),
             static_cast<int>(out.range),    static_cast<int>(out.matrix),
             static_cast<int>(out.primaries),static_cast<int>(out.transfer));

    return true;
}

bool VideoFilterGraph::needs_reconfigure(const AVFrame* in) const noexcept {
    if (!is_configured() || !in) return true;
    const VideoFilterInputParams& p = impl_->in_params;
    if (in->width != p.width || in->height != p.height) return true;
    if (static_cast<AVPixelFormat>(in->format) != p.pix_fmt) return true;

    // SAR: treat 0/1 (unset) as compatible -- many decoders never
    // populate SAR on individual frames.
    if (in->sample_aspect_ratio.num != p.sar.num ||
        in->sample_aspect_ratio.den != p.sar.den) {
        if (!(in->sample_aspect_ratio.num == 0 &&
              in->sample_aspect_ratio.den == 1)) {
            return true;
        }
    }

    // Colour metadata: treat the FFmpeg "UNSPECIFIED" sentinel on the
    // frame as compatible with whatever resolved value was passed to
    // configure().  Decoders routinely leave colour fields
    // UNSPECIFIED at the frame level when the bitstream lacks them;
    // the caller has already applied SD/HD/UHD heuristics, and we
    // should not force a reconfigure just because the frame still
    // carries the sentinel.
    auto compat = [](int frame_val, int cfg_val, int unspec) {
        return frame_val == cfg_val || frame_val == unspec;
    };
    if (!compat(in->color_range,     p.range,     AVCOL_RANGE_UNSPECIFIED))
        return true;
    if (!compat(in->colorspace,      p.matrix,    AVCOL_SPC_UNSPECIFIED))
        return true;
    if (!compat(in->color_primaries, p.primaries, AVCOL_PRI_UNSPECIFIED))
        return true;
    if (!compat(in->color_trc,       p.transfer,  AVCOL_TRC_UNSPECIFIED))
        return true;
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

    // Push the caller's frame as-is using KEEP_REF so libavfilter
    // takes its own reference and we do not mutate the caller's
    // frame.  Needed because needs_reconfigure() guarantees the
    // frame's metadata is already compatible with the buffer src,
    // so there is no need to clone-and-rewrite as earlier
    // revisions did -- doing so was both wasted work and a way
    // for genuinely mismatched metadata to silently slip through.
    //
    // Exception: HLG -> SDR needs a 203-nit mastering display side
    // data attached to the frame vf_scale receives.  Mutating the
    // caller's frame would be visible from the outside (and the
    // AVBufferRef of any pre-existing side data may be shared with
    // the upstream decoder cache).  Clone the frame -- a refcount
    // bump on the underlying pixel buffers, no copy -- and rewrite
    // the side data on the clone, then submit the clone (without
    // KEEP_REF, since we own this ref ourselves and free it right
    // after).
    AVFrame* in_clone = nullptr;
    if (impl_->need_hlg_sdr_fix &&
        in->color_trc == AVCOL_TRC_ARIB_STD_B67) {
        in_clone = av_frame_clone(in);
        if (!in_clone) {
            err = "av_frame_clone failed";
            return nullptr;
        }
        av_frame_remove_side_data(in_clone,
            AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
        AVMasteringDisplayMetadata* md =
            av_mastering_display_metadata_create_side_data(in_clone);
        if (md) {
            // 203 nits is the HLG SDR reference white -- the same
            // value libplacebo uses as PL_COLOR_SDR_WHITE.  With a
            // mastering display this small the HLG OOTF gamma
            // exponent collapses to ~1.0, so vf_scale stops trying
            // to tone map at all and we get back something close to
            // an SDR-tagged decode of the same Y'CbCr samples.
            md->max_luminance = av_make_q(203, 1);
            md->min_luminance = av_make_q(203, 10000);
            md->has_luminance = 1;
        }
    }

    AVFrame* submit = in_clone ? in_clone : const_cast<AVFrame*>(in);
    int submit_flags = in_clone ? 0 : AV_BUFFERSRC_FLAG_KEEP_REF;
    if (int ret = av_buffersrc_add_frame_flags(
            impl_->src_ctx, submit, submit_flags);
        ret < 0) {
        if (in_clone) av_frame_free(&in_clone);
        set_av_err(err, "buffersrc add_frame failed", ret);
        return nullptr;
    }
    if (in_clone) av_frame_free(&in_clone);

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

    // The output is now SDR; HDR side data on it would mislead any
    // downstream consumer that inspects per-frame metadata (e.g. an
    // RGB-uploader trying to second-guess the colour space).  Strip
    // it.  Only meaningful when the HLG->SDR fix is active; for
    // SDR->SDR or HDR->HDR pipelines vf_scale never produces these
    // side data fields anyway, so the calls are no-ops there.
    if (impl_->need_hlg_sdr_fix) {
        av_frame_remove_side_data(out,
            AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
        av_frame_remove_side_data(out,
            AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
    }
    return out;
}

} // namespace idiff

#endif // IDIFF_HAVE_FFMPEG
