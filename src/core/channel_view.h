#ifndef IDIFF_CHANNEL_VIEW_H
#define IDIFF_CHANNEL_VIEW_H

#include <opencv2/core.hpp>

#include <optional>
#include <string>

namespace idiff {

enum class ChannelViewMode {
    None,        // default, show all channels
    R,
    G,
    B,
    AlphaGray,
    AlphaBlend,
    AlphaContour,
    Y,
    U,
    V,
};

// Human-readable label for each mode, suitable for UI display.
const char* channel_view_mode_label(ChannelViewMode mode);

// Extract a single-channel or composited view from the source image.
// Returns nullopt when the requested mode is not applicable to the
// source format (e.g. AlphaGray on a 3-channel image).
// Output preserves the source bit depth (8 or 16 bit per channel).
std::optional<cv::Mat> extract_channel_view(const cv::Mat& src, ChannelViewMode mode);

} // namespace idiff

#endif // IDIFF_CHANNEL_VIEW_H
