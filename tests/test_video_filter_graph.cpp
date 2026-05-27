// SPDX-License-Identifier: BSD-2-Clause
//
// End-to-end pixel-level test of VideoFilterGraph.
//
// We synthesise a 64x64 YUV420P AVFrame with known limited-range
// BT.601 samples that decode to a specific RGB primary, push it
// through the wrapper used by VideoDecoder for the display path
// (vf_scale -> RGB24 sRGB), and verify the output pixels match the
// RGB primary within a tight tolerance.  If the wrapper passes the
// wrong matrix coefficients to libswscale -- which is the bug being
// hunted -- the output colour will be visibly wrong (e.g. red ->
// (255, ~24, 0) instead of (255, 0, 0)) and the test will fail.

#ifdef IDIFF_HAVE_FFMPEG

#include <catch2/catch_test_macros.hpp>

#include "core/video_filter_graph.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
}

#include <cstdint>
#include <cstring>
#include <string>

using namespace idiff;

namespace {

// Allocate a 64x64 YUV420P AVFrame and fill it with a single solid
// (Y, U, V) triple.  Caller takes ownership and must av_frame_free()
// the returned pointer.  Sets color tags on the frame so a graph
// configured from the frame's metadata sees the same values vf_scale
// will get.
AVFrame* make_solid_yuv420p_frame(int w, int h,
                                  uint8_t Y, uint8_t U, uint8_t V,
                                  AVColorRange range,
                                  AVColorSpace matrix,
                                  AVColorPrimaries primaries,
                                  AVColorTransferCharacteristic transfer) {
    AVFrame* f = av_frame_alloc();
    REQUIRE(f != nullptr);
    f->format = AV_PIX_FMT_YUV420P;
    f->width = w;
    f->height = h;
    REQUIRE(av_frame_get_buffer(f, 32) == 0);

    // Y plane
    for (int y = 0; y < h; ++y) {
        std::memset(f->data[0] + y * f->linesize[0], Y, w);
    }
    // U/V planes (subsampled 2x2)
    const int cw = w / 2;
    const int ch = h / 2;
    for (int y = 0; y < ch; ++y) {
        std::memset(f->data[1] + y * f->linesize[1], U, cw);
        std::memset(f->data[2] + y * f->linesize[2], V, cw);
    }

    f->color_range = range;
    f->colorspace = matrix;
    f->color_primaries = primaries;
    f->color_trc = transfer;
    f->sample_aspect_ratio = AVRational{1, 1};
    return f;
}

struct RgbSample { int r, g, b; };

RgbSample sample_center(const AVFrame* out) {
    REQUIRE(out != nullptr);
    REQUIRE(out->format == AV_PIX_FMT_RGB24);
    const int x = out->width / 2;
    const int y = out->height / 2;
    const uint8_t* p = out->data[0] + y * out->linesize[0] + x * 3;
    return {p[0], p[1], p[2]};
}

VideoFilterInputParams make_in_params(const AVFrame* f) {
    VideoFilterInputParams in;
    in.width = f->width;
    in.height = f->height;
    in.pix_fmt = static_cast<AVPixelFormat>(f->format);
    in.sar = f->sample_aspect_ratio.num
                 ? f->sample_aspect_ratio
                 : AVRational{1, 1};
    in.time_base = AVRational{1, 25};
    in.range = f->color_range;
    in.matrix = f->colorspace;
    in.primaries = f->color_primaries;
    in.transfer = f->color_trc;
    return in;
}

VideoFilterOutputParams make_srgb_rgb24_out() {
    VideoFilterOutputParams out;
    out.width = 0;
    out.height = 0;
    out.pix_fmt = AV_PIX_FMT_RGB24;
    out.range = AVCOL_RANGE_JPEG;
    out.matrix = AVCOL_SPC_RGB;
    out.primaries = AVCOL_PRI_BT709;
    out.transfer = AVCOL_TRC_IEC61966_2_1;
    return out;
}

} // namespace

// =============================================================================
// BT.601 limited-range pure red.
// =============================================================================
//
// `c=red` -> RGB(255,0,0), then `format=yuv420p` (default BT.601 limited)
// produces approximately (Y=81, U=90, V=240).  Pushing those samples
// back through vf_scale with in_color_matrix=BT.601 must reconstruct
// pure red within a small tolerance.
TEST_CASE("VideoFilterGraph: BT.601 limited red round-trips to (255,0,0)",
          "[video][filter_graph][color]") {
    AVFrame* in_frame = make_solid_yuv420p_frame(
        64, 64, 81, 90, 240,
        AVCOL_RANGE_MPEG,
        AVCOL_SPC_SMPTE170M,
        AVCOL_PRI_SMPTE170M,
        AVCOL_TRC_SMPTE170M);

    VideoFilterGraph graph;
    std::string err;
    REQUIRE(graph.configure(make_in_params(in_frame),
                            make_srgb_rgb24_out(), err));

    AVFrame* out = graph.process(in_frame, err);
    REQUIRE(out != nullptr);

    auto px = sample_center(out);
    INFO("RGB sample = (" << px.r << ", " << px.g << ", " << px.b << ")");

    // Tolerance accounts for limited-range quantisation and the
    // BT.601 -> sRGB transfer function's slight nonlinear bend
    // around the primaries.  These bounds are tight enough that a
    // wrong-matrix decode (e.g. BT.709) would still fail: BT.709
    // would produce roughly (255, ~24, 0) -- a green channel value
    // more than 10x our tolerance.
    CHECK(px.r >= 250);
    CHECK(px.g <= 5);
    CHECK(px.b <= 5);

    av_frame_free(&out);
    av_frame_free(&in_frame);
}

// =============================================================================
// BT.601 limited-range pure green.
// =============================================================================
//
// RGB(0,255,0) under BT.601 limited maps to roughly (Y=145, U=54, V=34).
TEST_CASE("VideoFilterGraph: BT.601 limited green round-trips to (0,255,0)",
          "[video][filter_graph][color]") {
    AVFrame* in_frame = make_solid_yuv420p_frame(
        64, 64, 145, 54, 34,
        AVCOL_RANGE_MPEG,
        AVCOL_SPC_SMPTE170M,
        AVCOL_PRI_SMPTE170M,
        AVCOL_TRC_SMPTE170M);

    VideoFilterGraph graph;
    std::string err;
    REQUIRE(graph.configure(make_in_params(in_frame),
                            make_srgb_rgb24_out(), err));

    AVFrame* out = graph.process(in_frame, err);
    REQUIRE(out != nullptr);

    auto px = sample_center(out);
    INFO("RGB sample = (" << px.r << ", " << px.g << ", " << px.b << ")");

    CHECK(px.r <= 5);
    CHECK(px.g >= 250);
    CHECK(px.b <= 5);

    av_frame_free(&out);
    av_frame_free(&in_frame);
}

// =============================================================================
// BT.601 limited-range pure blue.
// =============================================================================
//
// RGB(0,0,255) under BT.601 limited maps to roughly (Y=41, U=240, V=110).
TEST_CASE("VideoFilterGraph: BT.601 limited blue round-trips to (0,0,255)",
          "[video][filter_graph][color]") {
    AVFrame* in_frame = make_solid_yuv420p_frame(
        64, 64, 41, 240, 110,
        AVCOL_RANGE_MPEG,
        AVCOL_SPC_SMPTE170M,
        AVCOL_PRI_SMPTE170M,
        AVCOL_TRC_SMPTE170M);

    VideoFilterGraph graph;
    std::string err;
    REQUIRE(graph.configure(make_in_params(in_frame),
                            make_srgb_rgb24_out(), err));

    AVFrame* out = graph.process(in_frame, err);
    REQUIRE(out != nullptr);

    auto px = sample_center(out);
    INFO("RGB sample = (" << px.r << ", " << px.g << ", " << px.b << ")");

    CHECK(px.r <= 5);
    CHECK(px.g <= 5);
    CHECK(px.b >= 250);

    av_frame_free(&out);
    av_frame_free(&in_frame);
}

// =============================================================================
// BT.709 limited-range pure red.
// =============================================================================
//
// Same RGB(255,0,0) but encoded under BT.709 produces different YUV
// samples.  Y=63, U=102, V=240 (limited range).  vf_scale with
// in_color_matrix=BT.709 must reconstruct pure red.
TEST_CASE("VideoFilterGraph: BT.709 limited red round-trips to (255,0,0)",
          "[video][filter_graph][color]") {
    // BT.709 limited-range RGB(255,0,0):
    //   Y' = 16 + 0.2126*255*219/255 = 16 + 46.56 = 62.56 -> 63
    //   Cb = 128 - 0.1146*255 = 128 - 29.22 = 98.78 -> 99
    //   Cr = 128 + 0.5*255 = 128 + 127.5 = 255.5 -> 240 (limited range cap)
    // Use values close to the canonical encoded ones.
    AVFrame* in_frame = make_solid_yuv420p_frame(
        64, 64, 63, 102, 240,
        AVCOL_RANGE_MPEG,
        AVCOL_SPC_BT709,
        AVCOL_PRI_BT709,
        AVCOL_TRC_BT709);

    VideoFilterGraph graph;
    std::string err;
    REQUIRE(graph.configure(make_in_params(in_frame),
                            make_srgb_rgb24_out(), err));

    AVFrame* out = graph.process(in_frame, err);
    REQUIRE(out != nullptr);

    auto px = sample_center(out);
    INFO("RGB sample = (" << px.r << ", " << px.g << ", " << px.b << ")");

    CHECK(px.r >= 240);
    CHECK(px.g <= 15);
    CHECK(px.b <= 15);

    av_frame_free(&out);
    av_frame_free(&in_frame);
}

// =============================================================================
// Stride / stripe regression: non-aligned width, horizontal gradient.
// =============================================================================
//
// Stripes in the rendered Mat almost always come from a row-stride
// mismatch: libavfilter typically allocates output frames with
// linesize > width * bytes_per_pixel for SIMD alignment (e.g. width
// 70 -> linesize ~96 for RGB24).  If a downstream copy walks the
// destination Mat using width*bpp bytes per row but indexes the
// source at linesize stride per row (or vice versa), every row
// progressively shifts and produces visible diagonal stripes.
//
// Build a horizontal-gradient YUV420P frame at a width that vf_scale
// will definitely pad, push it through, then verify:
//   - linesize > width * 3 (i.e. the scenario is actually exercised);
//   - the output is a clean horizontal gradient -- adjacent rows
//     read identical pixel values at the same column.  Any stride
//     bug breaks this row-wise consistency.
TEST_CASE("VideoFilterGraph: non-aligned width round-trips without stripes",
          "[video][filter_graph][stride]") {
    constexpr int W = 70;   // not a multiple of 16/32: forces padding
    constexpr int H = 48;

    AVFrame* in_frame = av_frame_alloc();
    REQUIRE(in_frame != nullptr);
    in_frame->format = AV_PIX_FMT_YUV420P;
    in_frame->width = W;
    in_frame->height = H;
    REQUIRE(av_frame_get_buffer(in_frame, 32) == 0);

    // Horizontal grayscale ramp on Y, neutral chroma (U=V=128 ->
    // achromatic).  Each output column should therefore have a
    // stable luminance regardless of row.
    for (int y = 0; y < H; ++y) {
        uint8_t* row = in_frame->data[0] + y * in_frame->linesize[0];
        for (int x = 0; x < W; ++x) {
            // Limited range: keep within [16, 235].
            row[x] = static_cast<uint8_t>(16 + (x * 219) / (W - 1));
        }
    }
    for (int y = 0; y < H / 2; ++y) {
        std::memset(in_frame->data[1] + y * in_frame->linesize[1], 128, W / 2);
        std::memset(in_frame->data[2] + y * in_frame->linesize[2], 128, W / 2);
    }
    in_frame->color_range     = AVCOL_RANGE_MPEG;
    in_frame->colorspace      = AVCOL_SPC_BT709;
    in_frame->color_primaries = AVCOL_PRI_BT709;
    in_frame->color_trc       = AVCOL_TRC_BT709;
    in_frame->sample_aspect_ratio = AVRational{1, 1};

    VideoFilterGraph graph;
    std::string err;
    REQUIRE(graph.configure(make_in_params(in_frame),
                            make_srgb_rgb24_out(), err));

    AVFrame* out = graph.process(in_frame, err);
    REQUIRE(out != nullptr);
    REQUIRE(out->format == AV_PIX_FMT_RGB24);
    REQUIRE(out->width == W);
    REQUIRE(out->height == H);

    // Force the scenario we're trying to test: this assertion
    // documents that the frame really does have padding.  If a
    // future libavfilter tightens its allocation and removes the
    // padding, the regression we want to guard against can no
    // longer occur from this exact frame, and we should rewrite
    // the test rather than silently lose its value.
    REQUIRE(out->linesize[0] > W * 3);

    // Adjacent rows must agree byte-for-byte at every column,
    // because the input was a pure horizontal gradient.  Stride
    // bugs manifest as a one-pixel-per-row drift, so this catches
    // even subtle offset errors.
    bool rows_consistent = true;
    int first_bad_row = -1, first_bad_col = -1;
    for (int y = 1; y < H && rows_consistent; ++y) {
        const uint8_t* r0 = out->data[0] + 0 * out->linesize[0];
        const uint8_t* ry = out->data[0] + y * out->linesize[0];
        for (int x = 0; x < W * 3; ++x) {
            if (std::abs(int(r0[x]) - int(ry[x])) > 2) {
                rows_consistent = false;
                first_bad_row = y;
                first_bad_col = x / 3;
                break;
            }
        }
    }
    INFO("First inconsistency at row=" << first_bad_row
         << " col=" << first_bad_col
         << " (linesize=" << out->linesize[0] << ", width*3=" << (W * 3) << ")");
    CHECK(rows_consistent);

    // Also verify the gradient is monotonic across columns at the
    // middle row -- catches accidental column scrambling.
    bool cols_monotonic = true;
    const uint8_t* mid = out->data[0] + (H / 2) * out->linesize[0];
    for (int x = 1; x < W; ++x) {
        if (mid[x * 3] + 1 < mid[(x - 1) * 3]) {
            cols_monotonic = false;
            break;
        }
    }
    CHECK(cols_monotonic);

    av_frame_free(&out);
    av_frame_free(&in_frame);
}

// =============================================================================
// 10-bit YUV (yuv420p10le): stripes + colour reproduction.
// =============================================================================
//
// Decoded 10-bit content (HEVC Main10, AV1 yuv420p10le, etc.) lands
// at the buffer source as AV_PIX_FMT_YUV420P10LE: each sample is a
// 16-bit little-endian container with 10 valid bits (0..1023 nominal,
// limited-range Y in [64, 940]).  Two regression vectors at once:
//
//   1. Colour: vf_scale must drive its YUV->RGB matrix at 10-bit
//      precision, then dither to 8-bit RGB.  A typo or stale
//      bit-depth assumption shows up as totally wrong saturation
//      or hue.
//   2. Stripes: the 10LE plane has linesize >= 2*width, so any code
//      path that still computes "row_bytes = width" or otherwise
//      conflates element count with byte count produces a row-shift
//      every line -- the canonical diagonal-stripe artefact.
//
// We build a horizontal Y ramp (chroma neutral) at 10-bit precision
// and verify the output is a clean, monotonic gradient across rows.
TEST_CASE("VideoFilterGraph: 10-bit YUV420P10LE round-trips without stripes",
          "[video][filter_graph][10bit][color]") {
    constexpr int W = 64;
    constexpr int H = 48;

    AVFrame* in_frame = av_frame_alloc();
    REQUIRE(in_frame != nullptr);
    in_frame->format = AV_PIX_FMT_YUV420P10LE;
    in_frame->width = W;
    in_frame->height = H;
    REQUIRE(av_frame_get_buffer(in_frame, 32) == 0);

    // 10-bit limited-range Y: [64, 940].  Build a horizontal ramp.
    // Each Y sample is a 16-bit little-endian word, so the row stride
    // in *bytes* is linesize[0] and the row stride in *samples* is
    // linesize[0] / 2.
    for (int y = 0; y < H; ++y) {
        uint16_t* row = reinterpret_cast<uint16_t*>(
            in_frame->data[0] + y * in_frame->linesize[0]);
        for (int x = 0; x < W; ++x) {
            const uint16_t v =
                static_cast<uint16_t>(64 + (x * (940 - 64)) / (W - 1));
            row[x] = v;
        }
    }
    // Chroma neutral at 10-bit is 512 (== 128 << 2).
    const uint16_t chroma_neutral = 512;
    for (int y = 0; y < H / 2; ++y) {
        uint16_t* u = reinterpret_cast<uint16_t*>(
            in_frame->data[1] + y * in_frame->linesize[1]);
        uint16_t* v = reinterpret_cast<uint16_t*>(
            in_frame->data[2] + y * in_frame->linesize[2]);
        for (int x = 0; x < W / 2; ++x) {
            u[x] = chroma_neutral;
            v[x] = chroma_neutral;
        }
    }
    in_frame->color_range     = AVCOL_RANGE_MPEG;
    in_frame->colorspace      = AVCOL_SPC_BT709;
    in_frame->color_primaries = AVCOL_PRI_BT709;
    in_frame->color_trc       = AVCOL_TRC_BT709;
    in_frame->sample_aspect_ratio = AVRational{1, 1};

    VideoFilterGraph graph;
    std::string err;
    REQUIRE(graph.configure(make_in_params(in_frame),
                            make_srgb_rgb24_out(), err));

    AVFrame* out = graph.process(in_frame, err);
    REQUIRE(out != nullptr);
    REQUIRE(out->format == AV_PIX_FMT_RGB24);
    REQUIRE(out->width == W);
    REQUIRE(out->height == H);

    // (1) No stripes: rows must agree byte-for-byte because the
    //     input was a pure horizontal gradient.
    int first_bad_row = -1, first_bad_col = -1;
    bool rows_consistent = true;
    for (int y = 1; y < H && rows_consistent; ++y) {
        const uint8_t* r0 = out->data[0] + 0 * out->linesize[0];
        const uint8_t* ry = out->data[0] + y * out->linesize[0];
        for (int x = 0; x < W * 3; ++x) {
            if (std::abs(int(r0[x]) - int(ry[x])) > 3) {
                rows_consistent = false;
                first_bad_row = y;
                first_bad_col = x / 3;
                break;
            }
        }
    }
    INFO("10bit stripe check: first bad row=" << first_bad_row
         << " col=" << first_bad_col
         << " linesize=" << out->linesize[0]
         << " w*3=" << (W * 3));
    CHECK(rows_consistent);

    // (2) Achromatic gradient: R == G == B at every column (the
    //     chroma plane is exactly neutral).  Within tight tolerance.
    bool achromatic = true;
    int bad_col = -1, bad_dr = 0, bad_db = 0;
    const uint8_t* mid = out->data[0] + (H / 2) * out->linesize[0];
    for (int x = 0; x < W; ++x) {
        const int r = mid[x * 3 + 0];
        const int g = mid[x * 3 + 1];
        const int b = mid[x * 3 + 2];
        if (std::abs(r - g) > 4 || std::abs(g - b) > 4) {
            achromatic = false;
            bad_col = x;
            bad_dr = r - g;
            bad_db = g - b;
            break;
        }
    }
    INFO("10bit achromatic check: bad_col=" << bad_col
         << " r-g=" << bad_dr << " g-b=" << bad_db);
    CHECK(achromatic);

    // (3) Monotonic across columns.
    bool cols_monotonic = true;
    int prev = mid[0];
    for (int x = 1; x < W; ++x) {
        const int v = mid[x * 3];
        if (v + 2 < prev) {  // small tolerance for dither
            cols_monotonic = false;
            break;
        }
        prev = v;
    }
    CHECK(cols_monotonic);

    // (4) Endpoint sanity: leftmost ~ black (Y=64, limited), rightmost
    //     ~ white (Y=940, limited).
    const uint8_t* left  = mid + 0;
    const uint8_t* right = mid + (W - 1) * 3;
    INFO("Endpoints: left=(" << int(left[0]) << "," << int(left[1])
         << "," << int(left[2]) << ") right=(" << int(right[0]) << ","
         << int(right[1]) << "," << int(right[2]) << ")");
    CHECK(left[0]  <= 8);
    CHECK(right[0] >= 245);

    av_frame_free(&out);
    av_frame_free(&in_frame);
}

// =============================================================================
// SDR semantics: source primaries are honoured as-encoded.
// =============================================================================
//
// Regression guard.  An earlier refactor "simplified" the filter
// wrapper to always pass the full (range, matrix, primaries,
// transfer) four-tuple to vf_scale.  That sounds clean, but it
// silently changes meaning for SDR sources: vf_scale interprets a
// non-equal (src primaries, dst primaries) pair as a request for a
// gamut conversion, so a clip tagged SMPTE-170M played through a
// BT.709/sRGB sink produces e.g. red ~= (245, 41, 0) instead of
// pure red.  No consumer player does that for SDR display -- they
// all treat the source primaries as the destination's and only do
// matrix + range conversion.
//
// This test pins that "as encoded" behaviour: BT.601 limited red
// (Y=81, U=90, V=240) tagged with SMPTE-170M primaries / transfer
// must still come out near pure red, NOT shifted to the BT.709
// gamut-mapped equivalent.  If anyone re-introduces an
// always-full-four-tuple branch, this fails immediately.
TEST_CASE("VideoFilterGraph: SDR source primaries do not trigger gamut shift",
          "[video][filter_graph][color][sdr_semantics]") {
    AVFrame* in_frame = make_solid_yuv420p_frame(
        64, 64, 81, 90, 240,
        AVCOL_RANGE_MPEG,
        AVCOL_SPC_SMPTE170M,
        AVCOL_PRI_SMPTE170M,
        AVCOL_TRC_SMPTE170M);

    VideoFilterGraph graph;
    std::string err;
    REQUIRE(graph.configure(make_in_params(in_frame),
                            make_srgb_rgb24_out(), err));

    AVFrame* out = graph.process(in_frame, err);
    REQUIRE(out != nullptr);

    auto px = sample_center(out);
    INFO("RGB sample = (" << px.r << ", " << px.g << ", " << px.b << ")");

    // The "as encoded" decode lands at ~(255, 0, 0).  A gamut
    // conversion to BT.709 lands at ~(245, 41, 0); the green
    // channel is the discriminator -- 41 is more than 8x the
    // tolerance below and the test would fail loudly.
    CHECK(px.r >= 250);
    CHECK(px.g <= 5);
    CHECK(px.b <= 5);

    av_frame_free(&out);
    av_frame_free(&in_frame);
}

// =============================================================================
// graph_description() exposes the configured graph for debugging.
// =============================================================================
//
// Not a correctness test; a contract test for the debug/observability
// hook.  Keeping the public method covered means future refactors
// cannot quietly drop it.
TEST_CASE("VideoFilterGraph: graph_description reports a non-empty graph",
          "[video][filter_graph][debug]") {
    AVFrame* in_frame = make_solid_yuv420p_frame(
        64, 64, 128, 128, 128,
        AVCOL_RANGE_MPEG,
        AVCOL_SPC_BT709,
        AVCOL_PRI_BT709,
        AVCOL_TRC_BT709);

    VideoFilterGraph graph;
    std::string err;
    REQUIRE(graph.configure(make_in_params(in_frame),
                            make_srgb_rgb24_out(), err));

    const std::string desc = graph.graph_description();
    INFO(desc);
    CHECK(!desc.empty());
    // Sanity: the dump should mention the three named nodes we built.
    CHECK(desc.find("scale") != std::string::npos);
    CHECK(desc.find("in")    != std::string::npos);
    CHECK(desc.find("out")   != std::string::npos);

    av_frame_free(&in_frame);
}

#endif // IDIFF_HAVE_FFMPEG
