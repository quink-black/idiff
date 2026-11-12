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

// Scalar field that the reveal filter operates on.  Luma is the
// default; the chroma axes expose colour structure that is invisible in
// a luminance residual, and the Lab channels separate hue from
// brightness.  The chroma options only apply to colour sources: a gray
// image yields a flat mid-gray result.
enum class RevealChannel {
    Luma,      // Rec.601 luminance on code values
    Red,
    Green,
    Blue,
    ChromaRG,  // R - G, red/green opponent axis
    ChromaBY,  // B - (R + G) / 2, blue/yellow opponent axis
    LabA,      // CIELAB a*, shifted onto the 0..255 encoding
    LabB,      // CIELAB b*, shifted onto the 0..255 encoding
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
    // First field in the filter chain.  Chroma and Lab options expose
    // colour structure that a luminance residual cancels out.
    RevealChannel channel = RevealChannel::Luma;
    // Encode the residual against a blurred local magnitude instead of a
    // single global gain.  The two cannot share one gain: a flat
    // background carries about one 8-bit level of structure while
    // textured mid-tones such as clothing or hair carry an order of
    // magnitude more, so whichever the gain suits, the other either
    // saturates or stays invisible.  Normalisation keeps both readable.
    bool normalize = false;
    // Standard deviation of the local magnitude estimate, in pixels.
    // Keep it well above the feature scale so the divisor varies more
    // slowly than the structure being revealed.
    double normalize_sigma = 60.0;
    // Lower bound for the divisor, in 8-bit levels.  Without it a
    // perfectly flat area would divide by almost zero and turn
    // quantisation noise into visible grain.
    double normalize_floor = 0.12;
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
