#ifndef IDIFF_DEPTH_UTILS_H
#define IDIFF_DEPTH_UTILS_H

#include <opencv2/core.hpp>

namespace idiff {

// Convert any cv::Mat to CV_8U, preserving channel count.
// CV_8U:  pass-through (returns the input without copy).
// CV_16U: scale by 1.0/257.0 (maps 0-65535 to 0-255).
// CV_16S: shift to unsigned range then scale.
// CV_32F: clamp to [0,1] then scale to [0,255].
// Other:  best-effort convertTo(CV_8U).
cv::Mat convert_to_8u(const cv::Mat& src);

// Convert any cv::Mat to 4-channel CV_8UC4 (RGBA order) suitable for
// SDL texture upload.  Combines depth conversion and channel expansion
// in one call.  Returns an empty Mat on unsupported channel counts.
cv::Mat convert_to_rgba8(const cv::Mat& src);

// True when mat.depth() is not CV_8U (i.e. higher than 8-bit).
bool is_high_depth(const cv::Mat& m);

} // namespace idiff

#endif // IDIFF_DEPTH_UTILS_H
