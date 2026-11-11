#ifndef IDIFF_IMAGE_COMPARATOR_H
#define IDIFF_IMAGE_COMPARATOR_H

#include <memory>
#include <string>

#include "core/image.h"

namespace idiff {

enum class HeatmapColor {
    Gray,
    Inferno,
    Viridis,
    Coolwarm,
};

struct DifferenceOptions {
    // Amplification factor for difference values (1.0 = raw, 10.0 = 10x).
    double amplification = 5.0;
    // Minimum difference threshold (values below this are set to zero).
    int threshold = 0;
    // Heatmap color scheme for visualization.
    HeatmapColor heatmap_color = HeatmapColor::Inferno;
};

// Options for the single-image reveal filter.  The filter subtracts a
// blurred copy of the image from itself and maps the signed residual
// around mid-gray, which makes low-contrast structures (watermarks,
// baked-in overlays, tile seams) visible without a reference image.
struct RevealOptions {
    // Standard deviation of the background estimate, in pixels.  The
    // residual keeps only structures smaller than roughly this scale,
    // so the value should exceed the width of the feature being sought
    // and stay below the scale of the scene's lighting gradient.
    double background_sigma = 20.0;
    // Multiplier applied to the signed residual.  A gain of N maps a
    // residual of 128/N levels onto the full display range.
    double amplification = 20.0;
    // Suppress single-pixel noise on both sides of the subtraction so
    // faint structures are not buried in grain.
    bool smooth = true;
    // Color scheme.  Gray and Coolwarm preserve the sign of the
    // residual; the sequential maps do not.
    HeatmapColor heatmap_color = HeatmapColor::Gray;
};

class ImageComparator {
public:
    // Both images must have identical dimensions and pixel format.
    // Returns raw absolute difference image (same dimensions).
    std::unique_ptr<Image> compute_difference(const Image& a, const Image& b,
                                              const DifferenceOptions& options = {});

    // Generate a heatmap visualization of the difference.
    // Input is the raw difference image from compute_difference().
    std::unique_ptr<Image> compute_heatmap(const Image& diff,
                                           const DifferenceOptions& options = {});

    // Single-image reveal: subtract a Gaussian-blurred background from
    // the luminance of `src` and map the signed residual around mid-gray
    // with the requested amplification.  Unlike compute_difference(),
    // this needs no second image, so faint overlays can be inspected
    // when only one frame is available.
    std::unique_ptr<Image> compute_reveal(const Image& src,
                                          const RevealOptions& options = {});

    const std::string& last_error() const noexcept;

private:
    std::string last_error_;
};

} // namespace idiff

#endif // IDIFF_IMAGE_COMPARATOR_H
