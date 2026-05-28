#include "app/pixel_sampler.h"

#include "core/image.h"
#ifdef IDIFF_HAVE_FFMPEG
#include "core/image_impl.h"
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}
#endif

#include <opencv2/core.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace idiff {

namespace {

// "—" in UTF-8.  Stored once so format_pixel and format_delta produce
// byte-identical output when there is nothing to show.
constexpr const char* kEmDash = "\xE2\x80\x94";

void write_em_dash(char* buf, std::size_t n) {
    if (n == 0) return;
    std::snprintf(buf, n, "%s", kEmDash);
}

// Append "(v0, v1, ...)" or "v0" to buf using the supplied formatter.
// Always null-terminates when n > 0.  Returns the number of bytes
// written, capped at n - 1.
template <typename Fmt>
int append_channels(char* buf, std::size_t n, int channels, Fmt fmt) {
    if (n == 0 || channels <= 0) return 0;
    int written = 0;
    auto put = [&](const char* fragment) {
        if (static_cast<std::size_t>(written) >= n) return;
        int m = std::snprintf(buf + written, n - written, "%s", fragment);
        if (m > 0) written += std::min<int>(m, static_cast<int>(n - 1 - written));
    };
    if (channels == 1) {
        char tmp[32];
        fmt(0, tmp, sizeof(tmp));
        put(tmp);
        return written;
    }
    put("(");
    for (int i = 0; i < channels; ++i) {
        char tmp[32];
        fmt(i, tmp, sizeof(tmp));
        put(tmp);
        if (i + 1 < channels) put(", ");
    }
    put(")");
    return written;
}

// Channel-name prefix for a sample, e.g. "R G B: " for an 8U RGB
// triple or "Y Cb Cr: " for a YUV one.  Returns an empty string for
// PixelKind::Unknown so older call sites that never set `kind` keep
// their pre-existing zero-prefix output (and the existing test
// expectations remain valid).
const char* kind_prefix(PixelKind kind, int channels) {
    switch (kind) {
        case PixelKind::RGB:
            if (channels == 1) return "R: ";
            if (channels == 3) return "R G B: ";
            if (channels == 4) return "R G B A: ";
            return "";
        case PixelKind::YUV:
            if (channels == 1) return "Y: ";
            if (channels == 3) return "Y Cb Cr: ";
            return "";
        case PixelKind::Gray:
            if (channels == 1) return "Y: ";
            return "";
        case PixelKind::Unknown:
        default:
            return "";
    }
}

// Write `prefix` into buf and return how many bytes it consumed,
// leaving the rest of buf usable by the channel formatter.  Always
// null-terminates.  Returns 0 when prefix is empty or there is no
// room.
std::size_t write_prefix(char* buf, std::size_t n, const char* prefix) {
    if (n == 0 || !prefix || !*prefix) return 0;
    int m = std::snprintf(buf, n, "%s", prefix);
    if (m <= 0) return 0;
    return std::min<std::size_t>(static_cast<std::size_t>(m), n - 1);
}

} // namespace

PixelSample sample_image(const cv::Mat& m, double u, double v) {
    PixelSample s;
    if (m.empty()) return s;

    const int cols = m.cols;
    const int rows = m.rows;
    if (cols <= 0 || rows <= 0) return s;

    // Reject coords outside [0, 1).  Allowing u == 1.0 to round up to
    // `cols` would land on an out-of-range index after floor; the
    // contract states "out of range" returns invalid rather than silent
    // clamp.  Negative coords likewise fail.
    if (!(u >= 0.0) || !(v >= 0.0) || u >= 1.0 || v >= 1.0) return s;

    int px = static_cast<int>(std::floor(u * cols));
    int py = static_cast<int>(std::floor(v * rows));
    // floor on a finite [0, 1) input cannot exceed cols-1, but clamp
    // defensively to absorb floating-point drift on the boundary.
    if (px < 0) px = 0; else if (px >= cols) px = cols - 1;
    if (py < 0) py = 0; else if (py >= rows) py = rows - 1;

    const int channels = m.channels();
    if (channels < 1 || channels > 4) return s;

    s.valid = true;
    s.channels = channels;
    s.depth = m.depth();
    // The mats we see from VideoFileSource and ImageLoader are RGB24
    // or RGBA; gray-scale comes through as a single channel.  Tagging
    // here gives format_pixel enough context to label the columns.
    s.kind = (channels == 1) ? PixelKind::Gray : PixelKind::RGB;

    switch (s.depth) {
        case CV_8U: {
            const uint8_t* p = m.ptr<uint8_t>(py) + px * channels;
            for (int i = 0; i < channels; ++i) s.v[i] = static_cast<double>(p[i]);
            break;
        }
        case CV_16U: {
            const uint16_t* p = m.ptr<uint16_t>(py) + px * channels;
            for (int i = 0; i < channels; ++i) s.v[i] = static_cast<double>(p[i]);
            break;
        }
        case CV_32F: {
            const float* p = m.ptr<float>(py) + px * channels;
            for (int i = 0; i < channels; ++i) s.v[i] = static_cast<double>(p[i]);
            break;
        }
        default:
            // Leave the values untouched; format_pixel will surface the
            // unsupported depth diagnostically.  We still flag the
            // sample as valid so callers can distinguish "depth we
            // cannot format" from "no pixel at all".
            for (int i = 0; i < channels; ++i) s.v[i] = 0.0;
            break;
    }
    return s;
}

#ifdef IDIFF_HAVE_FFMPEG
namespace {

// Read one component value from an AVFrame using its
// AVPixFmtDescriptor entry.  `lx` / `ly` are coordinates in the
// component's own plane (already chroma-subsampled where appropriate).
// Container is either 1 byte/component or 2 bytes/component little-
// endian -- the only two layouts actually used by ffmpeg's planar /
// semi-planar formats.  Big-endian and 32-bit float component
// containers exist but never come out of a real-world video decode
// path; treat them as unsupported by returning -1 (caller falls back
// to the mat).
int read_component(const AVFrame* f,
                   const AVComponentDescriptor& c,
                   int comp_step_in_bytes,
                   int lx, int ly) {
    const uint8_t* plane = f->data[c.plane];
    if (!plane) return -1;
    const int linesize = f->linesize[c.plane];
    const uint8_t* row = plane + ly * linesize;
    const uint8_t* px = row + lx * comp_step_in_bytes + c.offset;

    int raw;
    if (c.depth <= 8) {
        raw = *px;
    } else if (c.depth <= 16) {
        // Little-endian 16-bit container; covers yuv420p10le, p010le,
        // yuv420p12le, and so on.  Big-endian variants are extremely
        // rare in decoded video; skip them.
        raw = static_cast<int>(px[0]) | (static_cast<int>(px[1]) << 8);
    } else {
        return -1;
    }
    return raw >> c.shift;
}

} // namespace
#endif // IDIFF_HAVE_FFMPEG

PixelSample sample_image_at(const Image* img, double u, double v,
                            bool prefer_rgb) {
    if (!img) return {};
    if (!(u >= 0.0) || !(v >= 0.0) || u >= 1.0 || v >= 1.0) return {};

#ifdef IDIFF_HAVE_FFMPEG
    // The native AVFrame path exposes original Y/Cb/Cr (or native
    // R/G/B) values at full source bit depth.  When the caller would
    // rather see the post-conversion 8-bit sRGB pixel that vf_scale
    // actually produced -- e.g. the inspector's "RGB" toggle for a
    // video source -- skip the AVFrame entirely and fall through to
    // the mat sampler below, which is always RGB24 in our pipeline.
    const AVFrame* f =
        prefer_rgb ? nullptr : img->internal().src_av_frame;
    if (f && f->width > 0 && f->height > 0 && f->data[0]) {
        const AVPixelFormat pf = static_cast<AVPixelFormat>(f->format);
        const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(pf);
        // Bail on layouts we cannot read with a uniform component
        // walker: hwaccel surfaces, bitstream codecs, paletted formats.
        // The mat fallback below handles them.
        if (d && !(d->flags & (AV_PIX_FMT_FLAG_HWACCEL |
                                AV_PIX_FMT_FLAG_BITSTREAM |
                                AV_PIX_FMT_FLAG_PAL)) &&
            d->nb_components >= 1 && d->nb_components <= 4) {
            // Map normalized (u, v) to luma-plane integer coords,
            // matching the convention used by sample_image() on mats.
            const int W = f->width;
            const int H = f->height;
            int lx = static_cast<int>(std::floor(u * W));
            int ly = static_cast<int>(std::floor(v * H));
            if (lx < 0) lx = 0; else if (lx >= W) lx = W - 1;
            if (ly < 0) ly = 0; else if (ly >= H) ly = H - 1;

            const bool is_rgb = (d->flags & AV_PIX_FMT_FLAG_RGB) != 0;
            // We expose Y/Cb/Cr or R/G/B but not alpha -- the inspector
            // table is already 3-column, and alpha on a video frame is
            // exotic.  Cap at the first 3 non-alpha components.
            int show_n = std::min<int>(d->nb_components, 3);
            // If the descriptor's last component is alpha, drop it.
            if ((d->flags & AV_PIX_FMT_FLAG_ALPHA) && show_n > 0) {
                show_n = std::min(show_n, d->nb_components - 1);
            }

            PixelSample s;
            int max_depth = 0;
            bool ok = true;
            for (int i = 0; i < show_n; ++i) {
                const AVComponentDescriptor& c = d->comp[i];
                if (c.depth > max_depth) max_depth = c.depth;

                // Chroma subsampling applies only to YUV components 1
                // and 2.  RGB and luma have shifts of 0 by definition.
                int cx = lx, cy = ly;
                if (!is_rgb && i > 0) {
                    cx = lx >> d->log2_chroma_w;
                    cy = ly >> d->log2_chroma_h;
                }

                int val = read_component(f, c, c.step, cx, cy);
                if (val < 0) { ok = false; break; }
                s.v[i] = static_cast<double>(val);
            }
            if (ok) {
                s.valid = true;
                s.channels = show_n;
                s.depth = (max_depth <= 8) ? CV_8U : CV_16U;
                s.kind = is_rgb ? PixelKind::RGB : PixelKind::YUV;
                return s;
            }
        }
    }
#else
    (void)prefer_rgb;
#endif

    // Fall back to the SDL-domain mat: still images, keyframe-scrub
    // previews, builds without FFmpeg, the prefer_rgb override above,
    // and any pix_fmt the walker declined to handle all land here.
    // The mat is always RGB24 in our pipeline, so sample_image already
    // tags it as RGB.
    return sample_image(img->mat(), u, v);
}

double pixel_to_norm(int x, int w) {
    if (w <= 0) return 0.5;
    if (x < 0) x = 0;
    if (x >= w) x = w - 1;
    return (static_cast<double>(x) + 0.5) / static_cast<double>(w);
}

int norm_to_pixel(double u, int w) {
    if (w <= 0) return 0;
    if (!(u >= 0.0)) u = 0.0;
    if (u >= 1.0) u = std::nextafter(1.0, 0.0);
    int px = static_cast<int>(std::floor(u * w));
    if (px < 0) px = 0;
    if (px >= w) px = w - 1;
    return px;
}

bool format_pixel(const PixelSample& s, char* buf, std::size_t n) {
    if (n == 0) return false;
    if (!s.valid) {
        write_em_dash(buf, n);
        return true;
    }

    const std::size_t off = write_prefix(buf, n, kind_prefix(s.kind, s.channels));
    char* tail = buf + off;
    std::size_t tail_n = n - off;

    switch (s.depth) {
        case CV_8U:
        case CV_16U: {
            // Both unsigned-integer depths print as plain decimal ints.
            // We round-to-nearest in case some upstream feeds us a
            // non-integer double, but in practice sample_image already
            // populated the array from integer pixel data.
            append_channels(tail, tail_n, s.channels,
                [&](int i, char* tmp, std::size_t tn) {
                    long long iv = static_cast<long long>(std::lround(s.v[i]));
                    std::snprintf(tmp, tn, "%lld", iv);
                });
            return true;
        }
        case CV_32F: {
            append_channels(tail, tail_n, s.channels,
                [&](int i, char* tmp, std::size_t tn) {
                    std::snprintf(tmp, tn, "%.4f", s.v[i]);
                });
            return true;
        }
        default:
            std::snprintf(buf, n, "(unsupported depth)");
            return false;
    }
}

namespace {

// Treat two PixelKinds as compatible-for-delta when they are equal,
// or when at least one side is Unknown (the back-compat path used by
// hand-built PixelSamples in older tests and pre-PixelKind call
// sites).  Cross-layout pairs like RGB vs YUV remain incomparable.
bool kinds_compatible(PixelKind a, PixelKind b) {
    if (a == PixelKind::Unknown || b == PixelKind::Unknown) return true;
    return a == b;
}

// Per-depth "fully opaque" alpha value used to back-fill the
// alpha channel on a 3-channel sample when its peer carries a 4th
// channel.  This matches the natural sRGB / PNG semantic that an
// image without an alpha channel is fully opaque, so the resulting
// alpha delta against a real RGBA pixel is "how far below opaque is
// the other side?", which is what users actually want to see when
// they line up an RGB24 PNG with a paletted (PAL8 + tRNS) PNG.
double opaque_alpha_for_depth(int depth) {
    switch (depth) {
        case CV_8U:  return 255.0;
        case CV_16U: return 65535.0;
        case CV_32F: return 1.0;
        default:     return 0.0;
    }
}

} // namespace

bool format_delta(const PixelSample& cur, const PixelSample& ref,
                  char* buf, std::size_t n) {
    if (n == 0) return false;
    if (!cur.valid || !ref.valid || cur.depth != ref.depth) {
        write_em_dash(buf, n);
        return false;
    }

    // Cross-kind comparisons (e.g. one side is native YUV from a
    // video frame, the other is RGB from an image or the prefer_rgb
    // mat path) are not directly comparable: subtracting an R channel
    // from a Y channel produces a number with no physical meaning.
    // Surface this the same way every other "not comparable" case is
    // surfaced -- as an em-dash -- so the user is not misled by a
    // signed integer that happens to look like a delta.  We treat
    // Unknown as "trust the caller" and let it through; that path is
    // exercised by hand-built PixelSamples in the older tests and by
    // call sites predating PixelKind.
    if (!kinds_compatible(cur.kind, ref.kind)) {
        write_em_dash(buf, n);
        return false;
    }

    // Channel-count handling.  Equal counts are the common case and
    // need no fix-up.  RGB(3) vs RGBA(4) -- in either order -- is the
    // realistic mixed case we care about: compare A's RGB24 PNG with
    // B's PAL8+tRNS PNG, or any other RGB <-> RGBA pair.  Back-fill
    // the missing alpha as "fully opaque" so the user sees an actual
    // delta (including how far the RGBA side is from opaque), tagged
    // with the wider "R G B A:" prefix to make the geometry obvious.
    // Anything else (e.g. 1 vs 3, 2 vs 4) stays an em-dash because
    // there is no defensible way to align the channels.
    int compare_n = cur.channels;
    bool rgb_vs_rgba =
        (cur.channels != ref.channels) &&
        ((cur.channels == 3 && ref.channels == 4) ||
         (cur.channels == 4 && ref.channels == 3)) &&
        // Only RGB layouts (or Unknown, treated as RGB-shaped) get the
        // alpha back-fill; YUV / Gray have no alpha concept.
        (cur.kind == PixelKind::RGB || cur.kind == PixelKind::Unknown) &&
        (ref.kind == PixelKind::RGB || ref.kind == PixelKind::Unknown);

    if (cur.channels != ref.channels) {
        if (!rgb_vs_rgba) {
            write_em_dash(buf, n);
            return false;
        }
        compare_n = 4;
    }

    auto get = [&](const PixelSample& s, int i) -> double {
        if (i < s.channels) return s.v[i];
        // i is past the source's channel count -- only happens for the
        // RGB(3)-vs-RGBA(4) bridge above, where the missing slot is
        // alpha.  Return the depth-appropriate "fully opaque" value.
        return opaque_alpha_for_depth(s.depth);
    };

    // Prefix uses the wider channel count so a 3-vs-4 delta still
    // renders as "R G B A:".  Pick whichever side advertises a known
    // kind for the layout label.
    PixelKind kind = (cur.kind != PixelKind::Unknown) ? cur.kind : ref.kind;
    const std::size_t off = write_prefix(buf, n, kind_prefix(kind, compare_n));
    char* tail = buf + off;
    std::size_t tail_n = n - off;

    switch (cur.depth) {
        case CV_8U:
        case CV_16U: {
            append_channels(tail, tail_n, compare_n,
                [&](int i, char* tmp, std::size_t tn) {
                    long long d = static_cast<long long>(std::lround(get(cur, i))) -
                                  static_cast<long long>(std::lround(get(ref, i)));
                    // Always show sign for non-zero deltas to make A/B
                    // direction obvious; zero stays unsigned to keep
                    // the common "identical pixel" case visually quiet.
                    if (d == 0) std::snprintf(tmp, tn, "0");
                    else        std::snprintf(tmp, tn, "%+lld", d);
                });
            return true;
        }
        case CV_32F: {
            append_channels(tail, tail_n, compare_n,
                [&](int i, char* tmp, std::size_t tn) {
                    double d = get(cur, i) - get(ref, i);
                    if (d == 0.0) std::snprintf(tmp, tn, "0.0000");
                    else          std::snprintf(tmp, tn, "%+.4f", d);
                });
            return true;
        }
        default:
            std::snprintf(buf, n, "(unsupported depth)");
            return false;
    }
}

} // namespace idiff
