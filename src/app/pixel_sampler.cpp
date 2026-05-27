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

PixelSample sample_image_at(const Image* img, double u, double v) {
    if (!img) return {};
    if (!(u >= 0.0) || !(v >= 0.0) || u >= 1.0 || v >= 1.0) return {};

#ifdef IDIFF_HAVE_FFMPEG
    const AVFrame* f = img->internal().src_av_frame;
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
#endif

    // Fall back to the SDL-domain mat: still images, keyframe-scrub
    // previews, builds without FFmpeg, and any pix_fmt the walker
    // above declined to handle all land here.  The mat is always
    // RGB24 in our pipeline, so sample_image already tags it as RGB.
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

    switch (s.depth) {
        case CV_8U:
        case CV_16U: {
            // Both unsigned-integer depths print as plain decimal ints.
            // We round-to-nearest in case some upstream feeds us a
            // non-integer double, but in practice sample_image already
            // populated the array from integer pixel data.
            append_channels(buf, n, s.channels,
                [&](int i, char* tmp, std::size_t tn) {
                    long long iv = static_cast<long long>(std::lround(s.v[i]));
                    std::snprintf(tmp, tn, "%lld", iv);
                });
            return true;
        }
        case CV_32F: {
            append_channels(buf, n, s.channels,
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

bool format_delta(const PixelSample& cur, const PixelSample& ref,
                  char* buf, std::size_t n) {
    if (n == 0) return false;
    if (!cur.valid || !ref.valid ||
        cur.channels != ref.channels ||
        cur.depth != ref.depth) {
        write_em_dash(buf, n);
        return false;
    }

    switch (cur.depth) {
        case CV_8U:
        case CV_16U: {
            append_channels(buf, n, cur.channels,
                [&](int i, char* tmp, std::size_t tn) {
                    long long d = static_cast<long long>(std::lround(cur.v[i])) -
                                  static_cast<long long>(std::lround(ref.v[i]));
                    // Always show sign for non-zero deltas to make A/B
                    // direction obvious; zero stays unsigned to keep
                    // the common "identical pixel" case visually quiet.
                    if (d == 0) std::snprintf(tmp, tn, "0");
                    else        std::snprintf(tmp, tn, "%+lld", d);
                });
            return true;
        }
        case CV_32F: {
            append_channels(buf, n, cur.channels,
                [&](int i, char* tmp, std::size_t tn) {
                    double d = cur.v[i] - ref.v[i];
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
