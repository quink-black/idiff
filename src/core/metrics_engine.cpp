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

std::optional<SingleImageMetrics> MetricsEngine::compute_single(const Image& img) {
    last_error_.clear();

    const auto& mat = img.mat();
    if (mat.empty()) {
        last_error_ = "Empty image";
        return std::nullopt;
    }

    try {
        SingleImageMetrics result;

        cv::Scalar mean, stddev;
        cv::meanStdDev(mat, mean, stddev);

        // OpenCV stores as BGR; map indices to our RGB fields
        if (mat.channels() >= 3) {
            int b_idx = 0, g_idx = 1, r_idx = 2;
            result.mean_r = mean[r_idx];
            result.mean_g = mean[g_idx];
            result.mean_b = mean[b_idx];
            result.var_r  = stddev[r_idx] * stddev[r_idx];
            result.var_g  = stddev[g_idx] * stddev[g_idx];
            result.var_b  = stddev[b_idx] * stddev[b_idx];
        } else {
            // Single channel: R = G = B
            result.mean_r = result.mean_g = result.mean_b = mean[0];
            result.var_r  = result.var_g  = result.var_b  = stddev[0] * stddev[0];
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
