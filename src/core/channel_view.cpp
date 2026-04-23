#include "core/channel_view.h"

#include <opencv2/imgproc.hpp>

namespace idiff {

const char* channel_view_mode_label(ChannelViewMode mode) {
    switch (mode) {
        case ChannelViewMode::None:         return "All";
        case ChannelViewMode::RGB:          return "RGB";
        case ChannelViewMode::R:            return "R";
        case ChannelViewMode::G:            return "G";
        case ChannelViewMode::B:            return "B";
        case ChannelViewMode::AlphaGray:    return "Alpha (Gray)";
        case ChannelViewMode::AlphaContour: return "Alpha Contour";
        case ChannelViewMode::Y:            return "Y";
        case ChannelViewMode::U:            return "U";
        case ChannelViewMode::V:            return "V";
    }
    return "All";
}

const char* view_background_label(ViewBackground bg) {
    switch (bg) {
        case ViewBackground::Black:        return "Black";
        case ViewBackground::White:        return "White";
        case ViewBackground::Red:          return "Red";
        case ViewBackground::Green:        return "Green";
        case ViewBackground::Blue:         return "Blue";
        case ViewBackground::DarkChecker:  return "Dark Checker";
        case ViewBackground::LightChecker: return "Light Checker";
    }
    return "Black";
}

namespace {

bool is_16bit(const cv::Mat& m) {
    return m.depth() == CV_16U;
}

cv::Mat extract_single_channel(const cv::Mat& src, int channel_idx) {
    std::vector<cv::Mat> channels;
    cv::split(src, channels);
    return channels[channel_idx];
}

// Composite RGBA over a solid color background.
// Returns RGBA8; 16-bit inputs are downsampled.
cv::Mat composite_solid(const cv::Mat& src, uint8_t br, uint8_t bg, uint8_t bb) {
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

    for (int y = 0; y < h; ++y) {
        const uint8_t* src_row = rgba8.ptr<uint8_t>(y);
        uint8_t* dst_row = dst.ptr<uint8_t>(y);

        for (int x = 0; x < w; ++x) {
            uint8_t r = src_row[x * 4 + 0];
            uint8_t g = src_row[x * 4 + 1];
            uint8_t b = src_row[x * 4 + 2];
            uint8_t a = src_row[x * 4 + 3];

            uint16_t inv_a = 255 - a;
            dst_row[x * 4 + 0] = static_cast<uint8_t>((r * a + br * inv_a) / 255);
            dst_row[x * 4 + 1] = static_cast<uint8_t>((g * a + bg * inv_a) / 255);
            dst_row[x * 4 + 2] = static_cast<uint8_t>((b * a + bb * inv_a) / 255);
            dst_row[x * 4 + 3] = 255;
        }
    }
    return dst;
}

// Composite RGBA over a checkerboard background.
// Returns RGBA8; 16-bit inputs are downsampled.
cv::Mat composite_checkerboard(const cv::Mat& src,
                                uint8_t light, uint8_t dark, int tile) {
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

    for (int y = 0; y < h; ++y) {
        const uint8_t* src_row = rgba8.ptr<uint8_t>(y);
        uint8_t* dst_row = dst.ptr<uint8_t>(y);
        bool row_dark = ((y / tile) & 1) == 1;

        for (int x = 0; x < w; ++x) {
            bool col_dark = ((x / tile) & 1) == 1;
            uint8_t bg = (row_dark ^ col_dark) ? dark : light;

            uint8_t r = src_row[x * 4 + 0];
            uint8_t g = src_row[x * 4 + 1];
            uint8_t b = src_row[x * 4 + 2];
            uint8_t a = src_row[x * 4 + 3];

            uint16_t inv_a = 255 - a;
            dst_row[x * 4 + 0] = static_cast<uint8_t>((r * a + bg * inv_a) / 255);
            dst_row[x * 4 + 1] = static_cast<uint8_t>((g * a + bg * inv_a) / 255);
            dst_row[x * 4 + 2] = static_cast<uint8_t>((b * a + bg * inv_a) / 255);
            dst_row[x * 4 + 3] = 255;
        }
    }
    return dst;
}

cv::Mat apply_background(const cv::Mat& src, ViewBackground bg) {
    switch (bg) {
        case ViewBackground::Black:
            return composite_solid(src, 0, 0, 0);
        case ViewBackground::White:
            return composite_solid(src, 255, 255, 255);
        case ViewBackground::Red:
            return composite_solid(src, 255, 0, 0);
        case ViewBackground::Green:
            return composite_solid(src, 0, 255, 0);
        case ViewBackground::Blue:
            return composite_solid(src, 0, 0, 255);
        case ViewBackground::DarkChecker:
            return composite_checkerboard(src, 48, 26, 8);
        case ViewBackground::LightChecker:
            return composite_checkerboard(src, 255, 204, 8);
    }
    return composite_solid(src, 0, 0, 0);
}

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

    cv::Mat alpha;
    cv::extractChannel(rgba8, alpha, 3);

    cv::Mat edges;
    cv::Canny(alpha, edges, 50, 150);

    cv::Mat dst(h, w, CV_8UC4);
    for (int y = 0; y < h; ++y) {
        const uint8_t* src_row = rgba8.ptr<uint8_t>(y);
        const uint8_t* edge_row = edges.ptr<uint8_t>(y);
        uint8_t* dst_row = dst.ptr<uint8_t>(y);

        for (int x = 0; x < w; ++x) {
            if (edge_row[x]) {
                dst_row[x * 4 + 0] = 255;
                dst_row[x * 4 + 1] = 0;
                dst_row[x * 4 + 2] = 0;
                dst_row[x * 4 + 3] = 255;
            } else {
                dst_row[x * 4 + 0] = static_cast<uint8_t>(src_row[x * 4 + 0] * 7 / 10);
                dst_row[x * 4 + 1] = static_cast<uint8_t>(src_row[x * 4 + 1] * 7 / 10);
                dst_row[x * 4 + 2] = static_cast<uint8_t>(src_row[x * 4 + 2] * 7 / 10);
                dst_row[x * 4 + 3] = src_row[x * 4 + 3];
            }
        }
    }
    return dst;
}

cv::Mat drop_alpha(const cv::Mat& src) {
    cv::Mat tmp(src.rows, src.cols,
                is_16bit(src) ? CV_16UC3 : CV_8UC3);
    int from_to[] = {0, 0, 1, 1, 2, 2};
    cv::mixChannels(&src, 1, &tmp, 1, from_to, 3);
    return tmp;
}

} // namespace

std::optional<cv::Mat> extract_channel_view(const cv::Mat& src,
                                             ChannelViewMode mode,
                                             ViewBackground bg) {
    if (src.empty()) {
        return std::nullopt;
    }

    const int channels = src.channels();
    const bool has_alpha = (channels == 4);

    switch (mode) {
        case ChannelViewMode::None:
            if (has_alpha) {
                return apply_background(src, bg);
            }
            return src.clone();

        case ChannelViewMode::RGB:
            if (has_alpha) {
                return drop_alpha(src);
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
            if (!has_alpha) return std::nullopt;
            return extract_single_channel(src, 3);

        case ChannelViewMode::AlphaContour:
            if (!has_alpha) return std::nullopt;
            return draw_alpha_contour(src);

        case ChannelViewMode::Y:
        case ChannelViewMode::U:
        case ChannelViewMode::V: {
            if (channels < 3) return std::nullopt;

            cv::Mat rgb = has_alpha ? drop_alpha(src) : src;

            cv::Mat yuv;
            if (is_16bit(rgb)) {
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
