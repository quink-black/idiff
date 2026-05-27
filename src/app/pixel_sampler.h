#ifndef IDIFF_APP_PIXEL_SAMPLER_H
#define IDIFF_APP_PIXEL_SAMPLER_H

#include <opencv2/core.hpp>

#include <cstddef>

namespace idiff {

class Image;

// Channel-layout hint for a sampled pixel.  Drives how format_pixel
// labels the components ("R G B: ..." vs "Y Cb Cr: ...") and how
// future tooling interprets the values.  Defaults to Unknown so
// existing call sites that only set valid/channels/depth/v keep
// rendering exactly as before.
enum class PixelKind {
    Unknown = 0,
    Gray,
    RGB,
    YUV,
};

// Per-channel sampled value.
//
// `valid` is the only field whose semantic depends on success: when
// false, the rest of the struct is left in an unspecified state and the
// caller MUST NOT use it for formatting or arithmetic.  When true,
// `channels` is in [1, 4], `depth` is one of CV_8U / CV_16U / CV_32F (or
// any other OpenCV depth -- formatters are responsible for rejecting
// unsupported depths), and `v[0..channels-1]` carries the sampled
// values, kept in `double` to avoid lossy conversions across the three
// supported bit-depths.
struct PixelSample {
    bool valid = false;
    int channels = 0;
    int depth = 0;       // cv::Mat::depth() value; CV_8U == 0
    double v[4] = {0.0, 0.0, 0.0, 0.0};
    PixelKind kind = PixelKind::Unknown;
};

// Sample the pixel under normalized coordinate (u, v).
//
// Mapping rule:  pix_x = floor(u * cols), then clamped to [0, cols-1];
// same for pix_y.  Empty matrices and any (u, v) with u < 0 or u >= 1
// (or the y counterpart) return `valid = false`.  Channel counts > 4
// are rejected; we never allocate.
PixelSample sample_image(const cv::Mat& m, double u, double v);

// Image-aware variant.  Prefers the source-domain AVFrame attached to
// the Image (native pix_fmt and bit depth) when one is present, so
// 10-bit and HDR sources are not silently truncated to the 8-bit
// RGB24 mat used for SDL upload.  Falls back to sample_image() on the
// Image's mat when no source frame is available (still images,
// keyframe scrub, builds without FFmpeg) -- the mat is RGB24 in that
// case, so the result is tagged PixelKind::RGB.  Returns valid=false
// when img is null or carries no usable pixel data.
PixelSample sample_image_at(const Image* img, double u, double v);

// Convert a native pixel coordinate to normalized [0, 1] using the
// pixel-center convention `(x + 0.5) / w`.  Out-of-range inputs are
// clamped to the valid integer range first so the result stays in
// (0, 1).  When w <= 0, returns 0.5 to keep callers from dividing by
// zero further upstream.
double pixel_to_norm(int x, int w);

// Inverse of pixel_to_norm: floor(u * w), clamped to [0, w-1].
// w <= 0 returns 0.
int norm_to_pixel(double u, int w);

// Render a PixelSample as text suitable for the inspector table.
// Returns true on success.  Unsupported depths produce
// "(unsupported depth)" and return false; the buffer is always
// null-terminated as long as `n > 0`.  When sample.valid is false,
// writes a single em-dash "—" (UTF-8) and returns true; this matches
// how the panel renders empty cells.
bool format_pixel(const PixelSample& s, char* buf, std::size_t n);

// Render `cur - ref` per channel with the same depth/channel rules as
// format_pixel.  Returns false (and writes "—") whenever the two
// samples are not directly comparable: either side invalid, channel
// count mismatch, or depth mismatch.  Equal samples produce zeros, not
// "—".
bool format_delta(const PixelSample& cur, const PixelSample& ref,
                  char* buf, std::size_t n);

} // namespace idiff

#endif // IDIFF_APP_PIXEL_SAMPLER_H
