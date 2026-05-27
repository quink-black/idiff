#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/metrics_engine.h"
#include "core/image_impl.h"

#include <opencv2/core.hpp>

using namespace idiff;
using Catch::Matchers::WithinAbs;

namespace {

std::unique_ptr<Image> make_image(cv::Mat mat,
                                  PixelFormat fmt = PixelFormat::RGB8) {
    auto img = std::make_unique<Image>();
    img->internal().info.width = mat.cols;
    img->internal().info.height = mat.rows;
    img->internal().info.pixel_format = fmt;
    img->internal().info.bit_depth =
        (fmt == PixelFormat::RGB16 || fmt == PixelFormat::Gray16 ||
         fmt == PixelFormat::RGBA16)
            ? 16
            : 8;
    img->internal().info.has_alpha =
        (fmt == PixelFormat::RGBA8 || fmt == PixelFormat::RGBA16);
    img->internal().mat = std::move(mat);
    return img;
}

}  // namespace

// -----------------------------------------------------------------------------
// Failure paths.
// -----------------------------------------------------------------------------

TEST_CASE("MetricsEngine: rejects invalid inputs", "[metrics]") {
    MetricsEngine engine;

    SECTION("compute with mismatched dimensions") {
        auto a = make_image(cv::Mat(10, 10, CV_8UC3, cv::Scalar(100, 100, 100)));
        auto b = make_image(cv::Mat(20, 20, CV_8UC3, cv::Scalar(100, 100, 100)));
        REQUIRE_FALSE(engine.compute(*a, *b).has_value());
        REQUIRE_FALSE(engine.last_error().empty());
    }
    SECTION("compute_single on an empty image") {
        Image empty;
        REQUIRE_FALSE(engine.compute_single(empty).has_value());
    }
    SECTION("compute_histogram on an empty image") {
        Image empty;
        REQUIRE_FALSE(engine.compute_histogram(empty).has_value());
    }
}

// -----------------------------------------------------------------------------
// Pairwise metrics.
// -----------------------------------------------------------------------------

TEST_CASE("MetricsEngine: identical images have zero MSE and SSIM 1.0",
          "[metrics]") {
    struct Case {
        PixelFormat fmt;
        int cv_type;
        cv::Scalar value;
    };
    auto c = GENERATE(
        Case{PixelFormat::RGB8, CV_8UC3, cv::Scalar(100, 150, 200)},
        Case{PixelFormat::RGB16, CV_16UC3, cv::Scalar(30000, 40000, 50000)});
    CAPTURE(static_cast<int>(c.fmt));

    cv::Mat data(32, 32, c.cv_type, c.value);
    auto a = make_image(data.clone(), c.fmt);
    auto b = make_image(data.clone(), c.fmt);

    MetricsEngine engine;

    auto mse = engine.compute_mse(*a, *b);
    REQUIRE(mse.has_value());
    REQUIRE_THAT(*mse, WithinAbs(0.0, 1e-6));

    auto ssim = engine.compute_ssim(*a, *b);
    REQUIRE(ssim.has_value());
    REQUIRE_THAT(*ssim, WithinAbs(1.0, 1e-3));
}

TEST_CASE(
    "MetricsEngine: different images yield finite PSNR and nonzero MSE/SSIM",
    "[metrics]") {
    auto a = make_image(cv::Mat(64, 64, CV_8UC3, cv::Scalar(100, 100, 100)));
    auto b = make_image(cv::Mat(64, 64, CV_8UC3, cv::Scalar(200, 200, 200)));

    MetricsEngine engine;

    // Individual accessors.
    auto mse = engine.compute_mse(*a, *b);
    REQUIRE(mse.has_value());
    REQUIRE(*mse > 0.0);

    auto psnr = engine.compute_psnr(*a, *b);
    REQUIRE(psnr.has_value());
    REQUIRE(*psnr > 0.0);
    REQUIRE(*psnr < 100.0);

    // Combined accessor returns all three metrics.
    auto all = engine.compute(*a, *b);
    REQUIRE(all.has_value());
    REQUIRE(all->mse > 0.0);
    REQUIRE(all->psnr > 0.0);
    REQUIRE(all->ssim > 0.0);
    REQUIRE(all->ssim <= 1.0);
}

// -----------------------------------------------------------------------------
// Single-image statistics.
// -----------------------------------------------------------------------------

TEST_CASE("MetricsEngine: compute_single on a uniform image",
          "[metrics]") {
    struct Case {
        PixelFormat fmt;
        int cv_type;
        cv::Scalar value;  // OpenCV stores BGR, so Scalar(B, G, R[, A]).
        double mean_r;
        double mean_g;
        double mean_b;
        double tol_mean;
        double tol_var;
    };
    auto c = GENERATE(
        // RGB8: BGR Scalar(30, 60, 90) -> mean_r=90, mean_g=60, mean_b=30.
        Case{PixelFormat::RGB8, CV_8UC3, cv::Scalar(30, 60, 90), 90.0, 60.0,
             30.0, 1e-3, 1e-6},
        // RGB16: BGR Scalar(10000, 30000, 60000) -> mean_r=60000, ...
        Case{PixelFormat::RGB16, CV_16UC3,
             cv::Scalar(10000, 30000, 60000), 60000.0, 30000.0, 10000.0,
             1.0, 1e-3},
        // Gray16: single channel mirrored to mean_r.
        Case{PixelFormat::Gray16, CV_16UC1, cv::Scalar(32768), 32768.0,
             32768.0, 32768.0, 1.0, 1e-3});
    CAPTURE(static_cast<int>(c.fmt));

    auto img = make_image(cv::Mat(4, 4, c.cv_type, c.value), c.fmt);

    MetricsEngine engine;
    auto r = engine.compute_single(*img);
    REQUIRE(r.has_value());

    REQUIRE_THAT(r->mean_r, WithinAbs(c.mean_r, c.tol_mean));
    REQUIRE_THAT(r->var_r, WithinAbs(0.0, c.tol_var));
    REQUIRE_THAT(r->min_r, WithinAbs(c.mean_r, c.tol_mean));
    REQUIRE_THAT(r->max_r, WithinAbs(c.mean_r, c.tol_mean));

    if (c.fmt != PixelFormat::Gray16) {
        REQUIRE_THAT(r->mean_g, WithinAbs(c.mean_g, c.tol_mean));
        REQUIRE_THAT(r->mean_b, WithinAbs(c.mean_b, c.tol_mean));
        REQUIRE_THAT(r->var_g, WithinAbs(0.0, c.tol_var));
        REQUIRE_THAT(r->var_b, WithinAbs(0.0, c.tol_var));
    }
}

TEST_CASE("MetricsEngine: compute_single recovers per-channel min/max",
          "[metrics]") {
    cv::Mat data(2, 2, CV_8UC3);
    // BGR Vec3b: pure-blue, pure-green, pure-red plus black.
    data.at<cv::Vec3b>(0, 0) = cv::Vec3b(0, 0, 0);
    data.at<cv::Vec3b>(0, 1) = cv::Vec3b(255, 0, 0);
    data.at<cv::Vec3b>(1, 0) = cv::Vec3b(0, 255, 0);
    data.at<cv::Vec3b>(1, 1) = cv::Vec3b(0, 0, 255);

    auto img = make_image(data);

    MetricsEngine engine;
    auto r = engine.compute_single(*img);
    REQUIRE(r.has_value());
    REQUIRE_THAT(r->min_r, WithinAbs(0.0, 1e-3));
    REQUIRE_THAT(r->max_r, WithinAbs(255.0, 1e-3));
    REQUIRE_THAT(r->min_g, WithinAbs(0.0, 1e-3));
    REQUIRE_THAT(r->max_g, WithinAbs(255.0, 1e-3));
    REQUIRE_THAT(r->min_b, WithinAbs(0.0, 1e-3));
    REQUIRE_THAT(r->max_b, WithinAbs(255.0, 1e-3));
}

// -----------------------------------------------------------------------------
// Histogram.
// -----------------------------------------------------------------------------

TEST_CASE("MetricsEngine: histogram bins uniform images at the right ends",
          "[metrics]") {
    struct Case {
        PixelFormat fmt;
        int cv_type;
        cv::Scalar value;
        int peak_bin;  // 255 for max, 0 for zero.
    };
    auto c = GENERATE(
        // RGB8: compute_histogram maps channel-0 to hist.r directly.
        // Scalar(255, 0, 0) -> hist.r peaks at 255, hist.g peaks at 0.
        Case{PixelFormat::RGB8, CV_8UC3, cv::Scalar(255, 0, 0), 255},
        // RGB16 saturated: 65535 -> 255 after normalization.
        Case{PixelFormat::RGB16, CV_16UC3,
             cv::Scalar(65535, 65535, 65535), 255},
        // RGB16 zero -> bin 0.
        Case{PixelFormat::RGB16, CV_16UC3, cv::Scalar(0, 0, 0), 0});
    CAPTURE(static_cast<int>(c.fmt), c.peak_bin);

    const int side = 8;
    auto img = make_image(cv::Mat(side, side, c.cv_type, c.value), c.fmt);

    MetricsEngine engine;
    auto hist = engine.compute_histogram(*img);
    REQUIRE(hist.has_value());

    REQUIRE(hist->r[c.peak_bin] == side * side);
    const int other = c.peak_bin == 255 ? 0 : 255;
    REQUIRE(hist->r[other] == 0);

    // RGB8 red: green channel peaks at bin 0 since G=0.
    if (c.fmt == PixelFormat::RGB8 && c.peak_bin == 255) {
        REQUIRE(hist->g[0] == side * side);
    }
}
