#include "core/channel_view.h"

#include <opencv2/imgproc.hpp>

namespace idiff {

const char* channel_view_mode_label(ChannelViewMode mode) {
    switch (mode) {
        case ChannelViewMode::None:       return "All";
        case ChannelViewMode::RGB:         return "RGB";
        case ChannelViewMode::R:          return "R";
        case ChannelViewMode::G:          return "G";
        case ChannelViewMode::B:          return "B";
        case ChannelViewMode::AlphaGray:  return "Alpha (Gray)";
        case ChannelViewMode::AlphaBlend: return "Alpha (Blend)";
        case ChannelViewMode::AlphaContour: return "Alpha Contour";
        case ChannelViewMode::Y:          return "Y";
        case ChannelViewMode::U:          return "U";
        case ChannelViewMode::V:          return "V";
    }
    return "All";
}

namespace {

// True if the mat is a 16-bit unsigned integer type.
bool is_16bit(const cv::Mat& m) {
    return m.depth() == CV_16U;
}

// Extract one channel by index, preserving bit depth.
cv::Mat extract_single_channel(const cv::Mat& src, int channel_idx) {
    std::vector<cv::Mat> channels;
    cv::split(src, channels);
    return channels[channel_idx];
}

// Composite RGBA over an 8x8 checkerboard background.
// Checker colors: white (255) and dark gray (51) to make transparency obvious.
// Returns RGBA8 regardless of input depth (alpha blending is inherently 8-bit
// display oriented; 16-bit inputs are downsampled to 8-bit for this view).
cv::Mat composite_alpha_blend(const cv::Mat& src) {
    CV_Assert(src.channels() == 4);

    cv::Mat rgba8;
    if (src.depth() == CV_16U) {
        src.convertTo(rgba8, CV_8UC4, 1.0 / 257.0);
    } else {
        rgba8 = src;
    }

    const int h = rgba8.rows;
    const int w = rgba8.cols;
    cv::Mat dst(h, w, CV_8UC4);

    constexpr uint8_t checker_light = 255;
    constexpr uint8_t checker_dark  = 51;
    constexpr int tile = 8;

    for (int y = 0; y < h; ++y) {
        const uint8_t* src_row = rgba8.ptr<uint8_t>(y);
        uint8_t* dst_row = dst.ptr<uint8_t>(y);
        bool row_dark = ((y / tile) & 1) == 1;

        for (int x = 0; x < w; ++x) {
            bool col_dark = ((x / tile) & 1) == 1;
            uint8_t bg = (row_dark ^ col_dark) ? checker_dark : checker_light;

            uint8_t r = src_row[x * 4 + 0];
            uint8_t g = src_row[x * 4 + 1];
            uint8_t b = src_row[x * 4 + 2];
            uint8_t a = src_row[x * 4 + 3];

            // Pre-multiplied alpha over solid background.
            uint16_t inv_a = 255 - a;
            dst_row[x * 4 + 0] = static_cast<uint8_t>((r * a + bg * inv_a) / 255);
            dst_row[x * 4 + 1] = static_cast<uint8_t>((g * a + bg * inv_a) / 255);
            dst_row[x * 4 + 2] = static_cast<uint8_t>((b * a + bg * inv_a) / 255);
            dst_row[x * 4 + 3] = 255;
        }
    }
    return dst;
}

// Overlay alpha channel edge contours on the RGB image so that misalignment
// between the alpha mask and the RGB content becomes immediately visible.
// The RGB image is slightly dimmed; bright red contour lines trace the edges
// of the alpha mask.  When alpha and RGB are aligned the contour hugs the
// object boundary; any offset is obvious.
cv::Mat draw_alpha_contour(const cv::Mat& src) {
    CV_Assert(src.channels() == 4);

    cv::Mat rgba8;
    if (src.depth() == CV_16U) {
        src.convertTo(rgba8, CV_8UC4, 1.0 / 257.0);
    } else {
        rgba8 = src;
    }

    const int h = rgba8.rows;
    const int w = rgba8.cols;

    // Extract alpha, detect edges.
    cv::Mat alpha;
    cv::extractChannel(rgba8, alpha, 3);

    cv::Mat edges;
    cv::Canny(alpha, edges, 50, 150);

    // Build output: dimmed RGB + alpha, then paint contour pixels red.
    cv::Mat dst(h, w, CV_8UC4);
    for (int y = 0; y < h; ++y) {
        const uint8_t* src_row = rgba8.ptr<uint8_t>(y);
        const uint8_t* edge_row = edges.ptr<uint8_t>(y);
        uint8_t* dst_row = dst.ptr<uint8_t>(y);

        for (int x = 0; x < w; ++x) {
            if (edge_row[x]) {
                // Bright red contour.
                dst_row[x * 4 + 0] = 255;
                dst_row[x * 4 + 1] = 0;
                dst_row[x * 4 + 2] = 0;
                dst_row[x * 4 + 3] = 255;
            } else {
                // Dim the RGB to 70 % so the red contour stands out.
                dst_row[x * 4 + 0] = static_cast<uint8_t>(src_row[x * 4 + 0] * 7 / 10);
                dst_row[x * 4 + 1] = static_cast<uint8_t>(src_row[x * 4 + 1] * 7 / 10);
                dst_row[x * 4 + 2] = static_cast<uint8_t>(src_row[x * 4 + 2] * 7 / 10);
                dst_row[x * 4 + 3] = src_row[x * 4 + 3];
            }
        }
    }
    return dst;
}

} // namespace

std::optional<cv::Mat> extract_channel_view(const cv::Mat& src, ChannelViewMode mode) {
    if (src.empty()) {
        return std::nullopt;
    }

    const int channels = src.channels();

    switch (mode) {
        case ChannelViewMode::None:
            return src.clone();

        case ChannelViewMode::RGB:
            if (channels == 4) {
                // RGBA -> RGB, drop alpha, preserve bit depth.
                cv::Mat tmp(src.rows, src.cols,
                            is_16bit(src) ? CV_16UC3 : CV_8UC3);
                int from_to[] = {0, 0, 1, 1, 2, 2};
                cv::mixChannels(&src, 1, &tmp, 1, from_to, 3);
                return tmp;
            }
            return src.clone();

        case ChannelViewMode::R:
            if (channels < 3) return std::nullopt;
            return extract_single_channel(src, 0);

        case ChannelViewMode::G:
            if (channels < 3) return std::nullopt;
            return extract_single_channel(src, 1);

        case ChannelViewMode::B:
            if (channels < 3) return std::nullopt;
            return extract_single_channel(src, 2);

        case ChannelViewMode::AlphaGray:
            if (channels != 4) return std::nullopt;
            return extract_single_channel(src, 3);

        case ChannelViewMode::AlphaBlend:
            if (channels != 4) return std::nullopt;
            return composite_alpha_blend(src);

        case ChannelViewMode::AlphaContour:
            if (channels != 4) return std::nullopt;
            return draw_alpha_contour(src);

        case ChannelViewMode::Y:
        case ChannelViewMode::U:
        case ChannelViewMode::V: {
            if (channels < 3) return std::nullopt;

            // Internal mats are RGB(A); drop alpha before YUV conversion.
            cv::Mat rgb;
            if (channels == 4) {
                // RGBA -> RGB preserves bit depth.
                cv::Mat tmp(src.rows, src.cols,
                            is_16bit(src) ? CV_16UC3 : CV_8UC3);
                int from_to[] = {0, 0, 1, 1, 2, 2};
                cv::mixChannels(&src, 1, &tmp, 1, from_to, 3);
                rgb = std::move(tmp);
            } else {
                rgb = src;
            }

            cv::Mat yuv;
            if (is_16bit(rgb)) {
                // OpenCV does not support 16-bit YUV conversion directly.
                // Downsample to 8-bit for the YUV view (this is a display
                // utility, not a precision analysis tool).
                cv::Mat rgb8;
                rgb.convertTo(rgb8, CV_8UC3, 1.0 / 257.0);
                cv::cvtColor(rgb8, yuv, cv::COLOR_RGB2YUV);
            } else {
                cv::cvtColor(rgb, yuv, cv::COLOR_RGB2YUV);
            }

            std::vector<cv::Mat> planes;
            cv::split(yuv, planes);

            int idx = (mode == ChannelViewMode::Y) ? 0
                    : (mode == ChannelViewMode::U) ? 1 : 2;
            return planes[idx];
        }
    }

    return std::nullopt;
}

} // namespace idiff
