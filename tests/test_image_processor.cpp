#include <catch2/catch_test_macros.hpp>

#include "core/image_processor.h"
#include "core/image_impl.h"

#include <opencv2/core.hpp>

using namespace idiff;

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

TEST_CASE("ImageProcessor: upscale with zero dimensions returns nullptr", "[image_processor]")
{
    auto src = make_image(cv::Mat(10, 10, CV_8UC3, cv::Scalar(128, 128, 128)));

    ImageProcessor proc;
    UpscaleOptions opts;
    opts.target_width = 0;
    opts.target_height = 0;

    auto result = proc.upscale(*src, opts);
    REQUIRE(result == nullptr);
    REQUIRE_FALSE(proc.last_error().empty());
}

TEST_CASE("ImageProcessor: upscale empty image returns nullptr", "[image_processor]")
{
    Image empty_img;
    ImageProcessor proc;
    UpscaleOptions opts;
    opts.target_width = 100;
    opts.target_height = 100;

    auto result = proc.upscale(empty_img, opts);
    REQUIRE(result == nullptr);
}

TEST_CASE("ImageProcessor: upscale produces correct dimensions", "[image_processor]")
{
    auto src = make_image(cv::Mat(32, 32, CV_8UC3, cv::Scalar(100, 150, 200)));

    ImageProcessor proc;
    UpscaleOptions opts;
    opts.target_width = 64;
    opts.target_height = 64;
    opts.method = UpscaleMethod::Lanczos;

    auto result = proc.upscale(*src, opts);
    REQUIRE(result != nullptr);
    REQUIRE(result->info().width == 64);
    REQUIRE(result->info().height == 64);
    REQUIRE(result->mat().cols == 64);
    REQUIRE(result->mat().rows == 64);
}

TEST_CASE("ImageProcessor: upscale preserves channel count", "[image_processor]")
{
    auto src = make_image(cv::Mat(16, 16, CV_8UC3, cv::Scalar(50, 100, 150)));

    ImageProcessor proc;
    UpscaleOptions opts;
    opts.target_width = 32;
    opts.target_height = 32;

    auto result = proc.upscale(*src, opts);
    REQUIRE(result != nullptr);
    REQUIRE(result->mat().channels() == 3);
}

TEST_CASE("ImageProcessor: upscale_to_match works when src is smaller", "[image_processor]")
{
    auto src = make_image(cv::Mat(32, 32, CV_8UC3, cv::Scalar(100, 100, 100)));
    auto ref = make_image(cv::Mat(64, 64, CV_8UC3, cv::Scalar(200, 200, 200)));

    ImageProcessor proc;
    auto result = proc.upscale_to_match(*src, *ref);
    REQUIRE(result != nullptr);
    REQUIRE(result->info().width == 64);
    REQUIRE(result->info().height == 64);
}

TEST_CASE("ImageProcessor: upscale_to_match fails when src is not smaller", "[image_processor]")
{
    auto src = make_image(cv::Mat(64, 64, CV_8UC3, cv::Scalar(100, 100, 100)));
    auto ref = make_image(cv::Mat(32, 32, CV_8UC3, cv::Scalar(200, 200, 200)));

    ImageProcessor proc;
    auto result = proc.upscale_to_match(*src, *ref);
    REQUIRE(result == nullptr);
}

TEST_CASE("ImageProcessor: all upscale methods produce valid output", "[image_processor]")
{
    auto src = make_image(cv::Mat(16, 16, CV_8UC3, cv::Scalar(128, 128, 128)));

    UpscaleMethod methods[] = {
        UpscaleMethod::Nearest,
        UpscaleMethod::Bilinear,
        UpscaleMethod::Bicubic,
        UpscaleMethod::Lanczos,
    };

    ImageProcessor proc;
    for (auto method : methods) {
        UpscaleOptions opts;
        opts.target_width = 48;
        opts.target_height = 48;
        opts.method = method;

        auto result = proc.upscale(*src, opts);
        REQUIRE(result != nullptr);
        REQUIRE(result->mat().cols == 48);
        REQUIRE(result->mat().rows == 48);
    }
}

TEST_CASE("ImageProcessor: upscale grayscale image", "[image_processor]")
{
    auto src = make_image(cv::Mat(16, 16, CV_8UC1, cv::Scalar(128)), PixelFormat::Gray8);

    ImageProcessor proc;
    UpscaleOptions opts;
    opts.target_width = 32;
    opts.target_height = 32;

    auto result = proc.upscale(*src, opts);
    REQUIRE(result != nullptr);
    REQUIRE(result->mat().channels() == 1);
    REQUIRE(result->mat().cols == 32);
}
