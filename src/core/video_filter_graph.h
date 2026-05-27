// SPDX-License-Identifier: BSD-2-Clause
#ifndef IDIFF_VIDEO_FILTER_GRAPH_H
#define IDIFF_VIDEO_FILTER_GRAPH_H

#ifdef IDIFF_HAVE_FFMPEG

#include <memory>
#include <string>

extern "C" {
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
}

struct AVFrame;

namespace idiff {

// Description of the buffer source feeding the filter graph.  Mirrors
// the fields that buffer src needs to be told at configure() time so
// libavfilter knows how to interpret incoming AVFrames.
struct VideoFilterInputParams {
    int width = 0;
    int height = 0;
    AVPixelFormat pix_fmt = AV_PIX_FMT_NONE;
    AVRational sar{1, 1};            // sample aspect ratio
    AVRational time_base{1, 1};

    // Source colour metadata, with UNSPECIFIED already resolved by the
    // caller (typically VideoColorTags::resolved_*()).  vf_scale needs
    // concrete values to do correct YUV->RGB / gamut conversion.
    AVColorRange range = AVCOL_RANGE_MPEG;
    AVColorSpace matrix = AVCOL_SPC_BT709;
    AVColorPrimaries primaries = AVCOL_PRI_BT709;
    AVColorTransferCharacteristic transfer = AVCOL_TRC_BT709;
};

// Description of the buffer sink: where the graph terminates and what
// pixel/colour layout the consumer expects.  When `width` / `height`
// are zero the graph keeps the source dimensions (vf_scale becomes a
// pure colour-conversion filter).
struct VideoFilterOutputParams {
    int width = 0;
    int height = 0;
    AVPixelFormat pix_fmt = AV_PIX_FMT_NONE;

    AVColorRange range = AVCOL_RANGE_JPEG;
    AVColorSpace matrix = AVCOL_SPC_RGB;
    AVColorPrimaries primaries = AVCOL_PRI_BT709;
    AVColorTransferCharacteristic transfer = AVCOL_TRC_IEC61966_2_1;
};

// Thin wrapper around a `buffer -> scale -> buffersink` filter graph.
// The graph is constructed once via configure() and then reused for
// every subsequent frame; reconfiguration is only required when the
// source frame's geometry, pixel format, or colour tags change.
//
// Why vf_scale rather than libswscale directly?  The filter version
// understands the full set of in_* / out_* colour properties (range,
// matrix, primaries, transfer) at the option level, which makes it
// trivial to wire any source colour space to any sink colour space
// without rebuilding swscale option strings by hand.  On the supported
// FFmpeg baseline (>= 8.1) the same filter also performs HDR tone
// mapping when the input transfer is PQ or HLG.
//
// HLG -> SDR fix-up: when the configured input transfer is HLG
// (ARIB-STD-B67) and the configured output transfer is a non-HDR
// transfer, process() injects a mastering-display side data with
// max_luminance = 203 nits onto the frame handed to vf_scale.  This
// collapses HLG's OOTF gamma to ~1.0 and effectively disables
// swscale's tone mapping, mirroring libplacebo's PL_COLOR_SDR_WHITE
// shortcut.  Without this, HLG midtones come out far too bright on an
// SDR sink (e.g. Y'=180 limited turns into R~253 instead of the
// SDR-baseline R~187, producing the washed-out, blown-out highlights
// users describe as "too bright / overly white").  The injection is
// done on a clone of the caller's frame; the caller's frame is left
// untouched.
//
// Thread safety: not thread-safe; serialise externally.
class VideoFilterGraph {
public:
    VideoFilterGraph();
    ~VideoFilterGraph();

    VideoFilterGraph(const VideoFilterGraph&) = delete;
    VideoFilterGraph& operator=(const VideoFilterGraph&) = delete;
    VideoFilterGraph(VideoFilterGraph&&) = delete;
    VideoFilterGraph& operator=(VideoFilterGraph&&) = delete;

    // Build (or rebuild) the graph for the given input/output params.
    // Returns true on success.  On failure, fills `err` with a short
    // human-readable reason and leaves the graph in a closed state.
    bool configure(const VideoFilterInputParams& in,
                   const VideoFilterOutputParams& out,
                   std::string& err);

    // Returns true if the graph has been successfully configured and
    // is ready to process frames.
    bool is_configured() const noexcept;

    // Compare a newly arrived frame against the params handed to the
    // last configure() call.  Returns true if anything material has
    // changed (dimensions, SAR, pixel format, or any of the four
    // colour fields) so the caller knows it must reconfigure before
    // process()ing.  Cheap; no allocations.
    bool needs_reconfigure(const AVFrame* in) const noexcept;

    // Push `in` through the graph and pull a single output frame.
    // Returns a reference-counted AVFrame on success (caller calls
    // av_frame_free() when done), or nullptr on failure.  The output
    // frame shares no buffers with `in`; vf_scale always produces a
    // freshly allocated output.  `in` is not modified, but the graph
    // may take an internal reference on it.
    //
    // Sets `err` only on failure.
    AVFrame* process(const AVFrame* in, std::string& err);

    // Tear down the graph and release all internal contexts.  After
    // reset() a fresh configure() call is required before process().
    void reset() noexcept;

    // Read-only access to the params last passed to configure().
    const VideoFilterInputParams& input_params() const noexcept;
    const VideoFilterOutputParams& output_params() const noexcept;

    // Human-readable dump of the configured graph (delegates to
    // FFmpeg's avfilter_graph_dump).  Returns an empty string when
    // the graph is not configured.  Intended for tests and debug
    // logging -- the format is whatever FFmpeg produces and is not
    // part of any contract.
    std::string graph_description() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace idiff

#endif // IDIFF_HAVE_FFMPEG
#endif // IDIFF_VIDEO_FILTER_GRAPH_H
