#ifndef IDIFF_CHANNEL_VIEW_H
#define IDIFF_CHANNEL_VIEW_H

#include <opencv2/core.hpp>

#include <optional>
#include <string>

namespace idiff {

enum class ChannelViewMode {
    None,         // show all channels
    RGB,          // drop alpha, show RGB only
    R,
    G,
    B,
    AlphaGray,
    AlphaContour,
    Y,
    U,
    V,
};

enum class ViewBackground {
    Black,
    White,
    Red,
    Green,
    Blue,
    DarkChecker,
    LightChecker,
};

const char* channel_view_mode_label(ChannelViewMode mode);
const char* view_background_label(ViewBackground bg);

// Extract a single-channel or composited view from the source image.
// Returns nullopt when the requested mode is not applicable to the
// source format (e.g. AlphaGray on a 3-channel image).
// Output preserves the source bit depth (8 or 16 bit per channel).
std::optional<cv::Mat> extract_channel_view(const cv::Mat& src,
                                             ChannelViewMode mode,
                                             ViewBackground bg);

// True if the requested mode requires an alpha channel and the source
// does not have one.
bool channel_view_requires_alpha(ChannelViewMode mode);

// Create a placeholder image indicating the source has no alpha channel.
// Returns an RGBA8 image of the requested size.
cv::Mat make_no_alpha_placeholder(int width, int height);

} // namespace idiff

#endif // IDIFF_CHANNEL_VIEW_H
