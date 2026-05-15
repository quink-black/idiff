#include "core/depth_utils.h"

#include <opencv2/imgproc.hpp>

namespace idiff {

cv::Mat convert_to_8u(const cv::Mat& src) {
    if (src.empty()) return src;

    switch (src.depth()) {
        case CV_8U:
            return src;

        case CV_16U: {
            cv::Mat dst;
            src.convertTo(dst, CV_8U, 1.0 / 257.0);
            return dst;
        }

        case CV_16S: {
            // Shift signed range [-32768, 32767] to unsigned [0, 65535],
            // then scale to [0, 255].
            cv::Mat u16;
            src.convertTo(u16, CV_16U, 1.0, 32768.0);
            cv::Mat dst;
            u16.convertTo(dst, CV_8U, 1.0 / 257.0);
            return dst;
        }

        case CV_32F: {
            // Assume [0, 1] range. Clamp then scale to [0, 255].
            cv::Mat clamped;
            cv::min(src, 1.0, clamped);
            cv::max(clamped, 0.0, clamped);
            cv::Mat dst;
            clamped.convertTo(dst, CV_8U, 255.0);
            return dst;
        }

        default: {
            cv::Mat dst;
            src.convertTo(dst, CV_8U);
            return dst;
        }
    }
}

cv::Mat convert_to_rgba8(const cv::Mat& src) {
    if (src.empty()) return {};

    cv::Mat u8 = convert_to_8u(src);
    int channels = u8.channels();

    cv::Mat rgba;
    switch (channels) {
        case 1:
            cv::cvtColor(u8, rgba, cv::COLOR_GRAY2RGBA);
            break;
        case 3:
            cv::cvtColor(u8, rgba, cv::COLOR_RGB2RGBA);
            break;
        case 4:
            rgba = u8;
            break;
        default:
            return {};
    }
    return rgba;
}

bool is_high_depth(const cv::Mat& m) {
    return m.depth() != CV_8U;
}

} // namespace idiff
