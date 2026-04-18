#include "core/metrics_engine.h"
#include "core/image_impl.h"

#include <cmath>
#include <limits>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/quality.hpp>

namespace idiff {

namespace {

bool validate_pair(const Image& a, const Image& b) {
    const auto& info_a = a.info();
    const auto& info_b = b.info();
    return info_a.width == info_b.width &&
           info_a.height == info_b.height &&
           info_a.pixel_format == info_b.pixel_format;
}

} // namespace

std::optional<MetricsResult> MetricsEngine::compute(const Image& a, const Image& b) {
    last_error_.clear();

    if (!validate_pair(a, b)) {
        last_error_ = "Images must have identical dimensions and pixel format";
        return std::nullopt;
    }

    MetricsResult result;

    auto psnr = compute_psnr(a, b);
    auto ssim = compute_ssim(a, b);
    auto mse = compute_mse(a, b);

    if (psnr) result.psnr = *psnr;
    if (ssim) result.ssim = *ssim;
    if (mse)  result.mse = *mse;

    return result;
}

std::optional<double> MetricsEngine::compute_psnr(const Image& a, const Image& b) {
    last_error_.clear();

    if (!validate_pair(a, b)) {
        last_error_ = "Images must have identical dimensions and pixel format";
        return std::nullopt;
    }

    try {
        auto quality = cv::quality::QualityPSNR::create(a.mat());
        cv::Scalar result = quality->compute(b.mat());
        return result[0];
    } catch (const cv::Exception& e) {
        last_error_ = e.what();
        return std::nullopt;
    }
}

std::optional<double> MetricsEngine::compute_ssim(const Image& a, const Image& b) {
    last_error_.clear();

    if (!validate_pair(a, b)) {
        last_error_ = "Images must have identical dimensions and pixel format";
        return std::nullopt;
    }

    try {
        auto quality = cv::quality::QualitySSIM::create(a.mat());
        cv::Scalar result = quality->compute(b.mat());
        return result[0];
    } catch (const cv::Exception& e) {
        last_error_ = e.what();
        return std::nullopt;
    }
}

std::optional<double> MetricsEngine::compute_mse(const Image& a, const Image& b) {
    last_error_.clear();

    if (!validate_pair(a, b)) {
        last_error_ = "Images must have identical dimensions and pixel format";
        return std::nullopt;
    }

    try {
        auto quality = cv::quality::QualityMSE::create(a.mat());
        cv::Scalar result = quality->compute(b.mat());
        return result[0];
    } catch (const cv::Exception& e) {
        last_error_ = e.what();
        return std::nullopt;
    }
}

namespace {

// Single-pass accumulator over 3 channels. Works for any scalar pixel type.
// Produces per-channel min/max/sum/sum-of-squares in one traversal of the data.
template <typename T>
struct ChannelStats {
    double sum[3] = {0, 0, 0};
    double sum_sq[3] = {0, 0, 0};
    double min_v[3] = {std::numeric_limits<double>::max(),
                       std::numeric_limits<double>::max(),
                       std::numeric_limits<double>::max()};
    double max_v[3] = {std::numeric_limits<double>::lowest(),
                       std::numeric_limits<double>::lowest(),
                       std::numeric_limits<double>::lowest()};

    void observe(int channel, T v) {
        const double dv = static_cast<double>(v);
        sum[channel] += dv;
        sum_sq[channel] += dv * dv;
        if (dv < min_v[channel]) min_v[channel] = dv;
        if (dv > max_v[channel]) max_v[channel] = dv;
    }
};

// OpenCV stores multi-channel BGR; our SingleImageMetrics fields are RGB.
// For 3-channel input, src channel 0=B, 1=G, 2=R.
template <typename T>
ChannelStats<T> accumulate_stats(const cv::Mat& mat) {
    ChannelStats<T> s;
    const int rows = mat.rows;
    const int cols = mat.cols;
    const int ch = mat.channels();
    if (ch == 1) {
        for (int y = 0; y < rows; ++y) {
            const T* row = mat.ptr<T>(y);
            for (int x = 0; x < cols; ++x) {
                s.observe(0, row[x]);
            }
        }
    } else {
        // Assume first 3 channels are B,G,R (ignore alpha if present).
        for (int y = 0; y < rows; ++y) {
            const T* row = mat.ptr<T>(y);
            for (int x = 0; x < cols; ++x) {
                const T* px = row + x * ch;
                s.observe(0, px[0]);
                s.observe(1, px[1]);
                s.observe(2, px[2]);
            }
        }
    }
    return s;
}

// Convert per-channel accumulators into SingleImageMetrics (RGB layout).
template <typename T>
SingleImageMetrics finalize_stats(const ChannelStats<T>& s, const cv::Mat& mat) {
    const double n = static_cast<double>(mat.rows) * static_cast<double>(mat.cols);
    SingleImageMetrics r;
    if (mat.channels() == 1) {
        const double mean = s.sum[0] / n;
        const double var = s.sum_sq[0] / n - mean * mean;
        r.mean_r = r.mean_g = r.mean_b = mean;
        r.var_r  = r.var_g  = r.var_b  = var;
        r.min_r  = r.min_g  = r.min_b  = s.min_v[0];
        r.max_r  = r.max_g  = r.max_b  = s.max_v[0];
        return r;
    }
    // BGR -> RGB mapping: channel 0=B, 1=G, 2=R.
    const double mean_b = s.sum[0] / n;
    const double mean_g = s.sum[1] / n;
    const double mean_r = s.sum[2] / n;
    r.mean_b = mean_b;
    r.mean_g = mean_g;
    r.mean_r = mean_r;
    // Population variance (matches cv::meanStdDev semantics), N (not N-1).
    r.var_b = s.sum_sq[0] / n - mean_b * mean_b;
    r.var_g = s.sum_sq[1] / n - mean_g * mean_g;
    r.var_r = s.sum_sq[2] / n - mean_r * mean_r;
    r.min_b = s.min_v[0]; r.min_g = s.min_v[1]; r.min_r = s.min_v[2];
    r.max_b = s.max_v[0]; r.max_g = s.max_v[1]; r.max_r = s.max_v[2];
    return r;
}

} // namespace

std::optional<SingleImageMetrics> MetricsEngine::compute_single(const Image& img) {
    last_error_.clear();

    const auto& mat = img.mat();
    if (mat.empty()) {
        last_error_ = "Empty image";
        return std::nullopt;
    }

    try {
        // Single-pass fast paths for the common depths. One traversal of the
        // pixel data produces mean/variance/min/max together, avoiding the
        // extra N*channels bytes of memory bandwidth that cv::split + per-
        // channel cv::minMaxLoc + cv::meanStdDev would require.
        switch (mat.depth()) {
            case CV_8U:
                return finalize_stats(accumulate_stats<uint8_t>(mat), mat);
            case CV_16U:
                return finalize_stats(accumulate_stats<uint16_t>(mat), mat);
            default:
                break;
        }

        // Fallback for other depths (CV_32F etc.) – keep the OpenCV route.
        cv::Scalar mean, stddev;
        cv::meanStdDev(mat, mean, stddev);

        std::vector<cv::Mat> channels;
        cv::split(mat, channels);

        double ch_min[3] = {}, ch_max[3] = {};
        const int nch = mat.channels();
        if (nch >= 3) {
            for (int c = 0; c < 3; ++c) {
                double lo, hi;
                cv::minMaxLoc(channels[c], &lo, &hi);
                ch_min[c] = lo;
                ch_max[c] = hi;
            }
        } else {
            double lo, hi;
            cv::minMaxLoc(channels[0], &lo, &hi);
            ch_min[0] = ch_min[1] = ch_min[2] = lo;
            ch_max[0] = ch_max[1] = ch_max[2] = hi;
        }

        SingleImageMetrics result;
        if (nch >= 3) {
            // OpenCV: [0]=B, [1]=G, [2]=R.
            result.mean_b = mean[0]; result.mean_g = mean[1]; result.mean_r = mean[2];
            result.var_b  = stddev[0] * stddev[0];
            result.var_g  = stddev[1] * stddev[1];
            result.var_r  = stddev[2] * stddev[2];
            result.min_b = ch_min[0]; result.min_g = ch_min[1]; result.min_r = ch_min[2];
            result.max_b = ch_max[0]; result.max_g = ch_max[1]; result.max_r = ch_max[2];
        } else {
            result.mean_r = result.mean_g = result.mean_b = mean[0];
            result.var_r  = result.var_g  = result.var_b  = stddev[0] * stddev[0];
            result.min_r  = result.min_g  = result.min_b  = ch_min[0];
            result.max_r  = result.max_g  = result.max_b  = ch_max[0];
        }
        return result;
    } catch (const cv::Exception& e) {
        last_error_ = e.what();
        return std::nullopt;
    }
}

std::optional<Histogram> MetricsEngine::compute_histogram(const Image& img) {
    last_error_.clear();

    const auto& mat = img.mat();
    if (mat.empty()) {
        last_error_ = "Empty image";
        return std::nullopt;
    }

    try {
        Histogram hist;

        std::vector<cv::Mat> channels;
        if (mat.channels() == 1) {
            channels.push_back(mat);
        } else {
            cv::split(mat, channels);
        }

        float range[] = {0, 256};
        const float* hist_range = {range};
        int hist_size = 256;

        for (int c = 0; c < static_cast<int>(channels.size()) && c < 3; ++c) {
            cv::Mat hist_mat;
            cv::calcHist(&channels[c], 1, 0, cv::Mat(), hist_mat, 1, &hist_size, &hist_range);

            uint32_t* target = nullptr;
            if (c == 0) target = hist.r.data();
            else if (c == 1) target = hist.g.data();
            else target = hist.b.data();

            for (int i = 0; i < hist_size; ++i) {
                target[i] = static_cast<uint32_t>(hist_mat.at<float>(i));
            }
        }

        return hist;
    } catch (const cv::Exception& e) {
        last_error_ = e.what();
        return std::nullopt;
    }
}

const std::string& MetricsEngine::last_error() const noexcept {
    return last_error_;
}

} // namespace idiff
