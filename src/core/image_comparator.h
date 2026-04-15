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

    const std::string& last_error() const noexcept;

private:
    std::string last_error_;
};

} // namespace idiff

#endif // IDIFF_IMAGE_COMPARATOR_H
