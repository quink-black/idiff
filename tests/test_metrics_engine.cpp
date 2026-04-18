#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/metrics_engine.h"
#include "core/image_impl.h"

#include <opencv2/core.hpp>

using namespace idiff;
using Catch::Matchers::WithinAbs;

static std::unique_ptr<Image> make_image(cv::Mat mat, PixelFormat fmt = PixelFormat::RGB8) {
    auto img = std::make_unique<Image>();
    img->internal().info.width = mat.cols;
    img->internal().info.height = mat.rows;
    img->internal().info.pixel_format = fmt;
    img->internal().info.bit_depth = 8;
    img->internal().info.has_alpha = false;
    img->internal().mat = std::move(mat);
    return img;
}

TEST_CASE("MetricsEngine: dimension mismatch returns nullopt", "[metrics]")
{
    auto a = make_image(cv::Mat(10, 10, CV_8UC3, cv::Scalar(100, 100, 100)));
    auto b = make_image(cv::Mat(20, 20, CV_8UC3, cv::Scalar(100, 100, 100)));

    MetricsEngine engine;
    auto result = engine.compute(*a, *b);
    REQUIRE_FALSE(result.has_value());
    REQUIRE_FALSE(engine.last_error().empty());
}

TEST_CASE("MetricsEngine: identical images have infinite PSNR and zero MSE", "[metrics]")
{
    cv::Mat data(64, 64, CV_8UC3, cv::Scalar(100, 150, 200));
    auto a = make_image(data.clone());
    auto b = make_image(data.clone());

    MetricsEngine engine;

    auto mse = engine.compute_mse(*a, *b);
    REQUIRE(mse.has_value());
    REQUIRE_THAT(*mse, WithinAbs(0.0, 1e-6));

    auto ssim = engine.compute_ssim(*a, *b);
    REQUIRE(ssim.has_value());
    REQUIRE_THAT(*ssim, WithinAbs(1.0, 1e-3));
}

TEST_CASE("MetricsEngine: different images have finite PSNR and nonzero MSE", "[metrics]")
{
    auto a = make_image(cv::Mat(64, 64, CV_8UC3, cv::Scalar(100, 100, 100)));
    auto b = make_image(cv::Mat(64, 64, CV_8UC3, cv::Scalar(200, 200, 200)));

    MetricsEngine engine;

    auto mse = engine.compute_mse(*a, *b);
    REQUIRE(mse.has_value());
    REQUIRE(*mse > 0.0);

    auto psnr = engine.compute_psnr(*a, *b);
    REQUIRE(psnr.has_value());
    REQUIRE(*psnr > 0.0);
    REQUIRE(*psnr < 100.0);  // Should be finite for different images
}

TEST_CASE("MetricsEngine: compute returns all three metrics", "[metrics]")
{
    auto a = make_image(cv::Mat(32, 32, CV_8UC3, cv::Scalar(50, 100, 150)));
    auto b = make_image(cv::Mat(32, 32, CV_8UC3, cv::Scalar(60, 110, 160)));

    MetricsEngine engine;
    auto result = engine.compute(*a, *b);
    REQUIRE(result.has_value());
    REQUIRE(result->mse > 0.0);
    REQUIRE(result->psnr > 0.0);
    REQUIRE(result->ssim > 0.0);
    REQUIRE(result->ssim <= 1.0);
}

TEST_CASE("MetricsEngine: histogram on valid image", "[metrics]")
{
    // Create a solid red image (RGB order: R=255, G=0, B=0)
    auto img = make_image(cv::Mat(32, 32, CV_8UC3, cv::Scalar(255, 0, 0)));

    MetricsEngine engine;
    auto hist = engine.compute_histogram(*img);
    REQUIRE(hist.has_value());

    // First channel (R) should have all pixels at bin 255
    REQUIRE(hist->r[255] == 32 * 32);
    REQUIRE(hist->r[0] == 0);

    // Second channel (G) should have all pixels at bin 0
    REQUIRE(hist->g[0] == 32 * 32);
}

TEST_CASE("MetricsEngine: histogram on empty image returns nullopt", "[metrics]")
{
    Image empty_img;
    MetricsEngine engine;
    auto hist = engine.compute_histogram(empty_img);
    REQUIRE_FALSE(hist.has_value());
}

TEST_CASE("MetricsEngine: compute_single returns mean/var/min/max", "[metrics]")
{
    // BGR order in OpenCV: Scalar(B, G, R)
    cv::Mat data(4, 4, CV_8UC3, cv::Scalar(30, 60, 90));
    auto img = make_image(data);

    MetricsEngine engine;
    auto result = engine.compute_single(*img);
    REQUIRE(result.has_value());

    // All pixels are identical, so mean = pixel value, var = 0, min = max = pixel value
    REQUIRE_THAT(result->mean_r, WithinAbs(90.0, 1e-3));
    REQUIRE_THAT(result->mean_g, WithinAbs(60.0, 1e-3));
    REQUIRE_THAT(result->mean_b, WithinAbs(30.0, 1e-3));

    REQUIRE_THAT(result->var_r, WithinAbs(0.0, 1e-6));
    REQUIRE_THAT(result->var_g, WithinAbs(0.0, 1e-6));
    REQUIRE_THAT(result->var_b, WithinAbs(0.0, 1e-6));

    REQUIRE_THAT(result->min_r, WithinAbs(90.0, 1e-3));
    REQUIRE_THAT(result->max_r, WithinAbs(90.0, 1e-3));
    REQUIRE_THAT(result->min_g, WithinAbs(60.0, 1e-3));
    REQUIRE_THAT(result->max_g, WithinAbs(60.0, 1e-3));
    REQUIRE_THAT(result->min_b, WithinAbs(30.0, 1e-3));
    REQUIRE_THAT(result->max_b, WithinAbs(30.0, 1e-3));
}

TEST_CASE("MetricsEngine: compute_single min/max on varying image", "[metrics]")
{
    // Create an image with known range per channel using a gradient
    cv::Mat data(2, 2, CV_8UC3);
    // BGR: top-left (0,0,0), top-right (255,0,0), bottom-left (0,255,0), bottom-right (0,0,255)
    data.at<cv::Vec3b>(0, 0) = cv::Vec3b(0, 0, 0);
    data.at<cv::Vec3b>(0, 1) = cv::Vec3b(255, 0, 0);
    data.at<cv::Vec3b>(1, 0) = cv::Vec3b(0, 255, 0);
    data.at<cv::Vec3b>(1, 1) = cv::Vec3b(0, 0, 255);

    auto img = make_image(data);

    MetricsEngine engine;
    auto result = engine.compute_single(*img);
    REQUIRE(result.has_value());

    // B=channel: values 0,255,0,0 -> min=0, max=255
    // G=channel: values 0,0,255,0 -> min=0, max=255
    // R=channel: values 0,0,0,255 -> min=0, max=255
    REQUIRE_THAT(result->min_r, WithinAbs(0.0, 1e-3));
    REQUIRE_THAT(result->max_r, WithinAbs(255.0, 1e-3));
    REQUIRE_THAT(result->min_g, WithinAbs(0.0, 1e-3));
    REQUIRE_THAT(result->max_g, WithinAbs(255.0, 1e-3));
    REQUIRE_THAT(result->min_b, WithinAbs(0.0, 1e-3));
    REQUIRE_THAT(result->max_b, WithinAbs(255.0, 1e-3));
}

TEST_CASE("MetricsEngine: compute_single on empty image returns nullopt", "[metrics]")
{
    Image empty_img;
    MetricsEngine engine;
    auto result = engine.compute_single(empty_img);
    REQUIRE_FALSE(result.has_value());
}
