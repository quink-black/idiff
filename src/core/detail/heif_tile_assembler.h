// SPDX-License-Identifier: BSD-2-Clause
#ifndef IDIFF_HEIF_TILE_ASSEMBLER_H
#define IDIFF_HEIF_TILE_ASSEMBLER_H

#ifdef IDIFF_HAVE_FFMPEG_IMAGE_DECODE

#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
}

struct AVFrame;
struct AVStreamGroupTileGrid;

namespace idiff {

// Per-tile buffer source description handed to configure().  The
// assembler builds one `buffer` filter per entry, all feeding into a
// single xstack node whose layout mirrors the on-disk tile grid.
struct HeifTileInputDesc {
    int width = 0;
    int height = 0;
    AVPixelFormat pix_fmt = AV_PIX_FMT_NONE;
    AVRational sar{1, 1};
    AVRational time_base{1, 1};

    // Source color description.  All tiles of one HEIF image share a
    // single description; it is set on every buffer source so the
    // graph interprets chroma correctly, and stamped onto the composed
    // output frame so the downstream YUV->RGB conversion can honour
    // the source matrix / primaries / transfer.  Defaults are the
    // FFmpeg UNSPECIFIED sentinels.
    AVColorRange range = AVCOL_RANGE_UNSPECIFIED;
    AVColorSpace matrix = AVCOL_SPC_UNSPECIFIED;
    AVColorPrimaries primaries = AVCOL_PRI_UNSPECIFIED;
    AVColorTransferCharacteristic transfer = AVCOL_TRC_UNSPECIFIED;
};

// Wrapper around a one-shot HEIF tile-grid composition graph:
//
//   [in0][in1]...[in_{N-1}]
//     xstack=inputs=N:layout=...:fill=0xRRGGBB@0xAA,
//     crop=W:H:hoff:voff,
//     format=<sink_pix_fmt>
//   [out]
//
// HEIF / AVIF demuxers expose every tile as an independent AVStream
// plus an AVStreamGroupTileGrid descriptor.  libav* itself never
// composes the tiles; assembling the full image is the caller's job.
// idiff defers to libavfilter's xstack here for two reasons: it is
// the same path fftools uses (so behaviour matches the ffmpeg CLI on
// edge cases like non-grid offsets and partial-tile padding), and it
// trivially handles 4:2:0 / 4:2:2 / 4:4:4 chroma subsampling and
// 10/12-bit depths without idiff having to re-implement the chroma
// alignment rules.
//
// Use:
//   HeifTileAssembler asm;
//   if (!asm.configure(grid, inputs, AV_PIX_FMT_RGB24, err)) ...
//   AVFrame* big = asm.assemble(tile_frames, err);
//
// `inputs` must have exactly grid->nb_tiles entries, in the same
// order as grid->offsets[].  `tile_frames` passed to assemble() must
// match the inputs by index.
//
// Thread safety: not thread-safe; serialise externally.
class HeifTileAssembler {
public:
    HeifTileAssembler();
    ~HeifTileAssembler();

    HeifTileAssembler(const HeifTileAssembler&) = delete;
    HeifTileAssembler& operator=(const HeifTileAssembler&) = delete;
    HeifTileAssembler(HeifTileAssembler&&) = delete;
    HeifTileAssembler& operator=(HeifTileAssembler&&) = delete;

    // Build the graph for a particular grid.  `out_pix_fmt` is the
    // pixel format the buffersink is constrained to.  Callers that
    // want correct color handling should pass the tiles' *native*
    // pixel format here so composition stays in the source color
    // space; the composed output frame carries the source color tags
    // (from inputs[0]) and is converted to display RGB by a later
    // VideoFilterGraph pass.  Returns true on success; on failure,
    // fills `err` and leaves the assembler in a closed state.
    bool configure(const AVStreamGroupTileGrid* grid,
                   const std::vector<HeifTileInputDesc>& inputs,
                   AVPixelFormat out_pix_fmt,
                   std::string& err);

    bool is_configured() const noexcept;

    // Push one tile per buffersrc, then pull a single output frame.
    // `tiles` must have the same length and order as the `inputs`
    // vector previously handed to configure().  The caller retains
    // ownership of `tiles[i]`; the graph takes its own refs.
    //
    // Returns a freshly allocated AVFrame on success (caller must
    // av_frame_free()), or nullptr on failure with `err` filled in.
    AVFrame* assemble(const std::vector<AVFrame*>& tiles,
                      std::string& err);

    // Tear down the graph and release all internal contexts.
    void reset() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace idiff

#endif // IDIFF_HAVE_FFMPEG_IMAGE_DECODE
#endif // IDIFF_HEIF_TILE_ASSEMBLER_H
