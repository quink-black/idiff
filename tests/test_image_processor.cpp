#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "core/image_processor.h"
#include "core/image_impl.h"

#include <opencv2/core.hpp>

using namespace idiff;

namespace {

std::unique_ptr<Image> make_image(cv::Mat mat,
                                  PixelFormat fmt = PixelFormat::RGB8) {
    auto img = std::make_unique<Image>();
    img->internal().info.width = mat.cols;
    img->internal().info.height = mat.rows;
    img->internal().info.pixel_format = fmt;
    img->internal().info.bit_depth = 8;
    img->internal().info.has_alpha = false;
    img->internal().mat = std::move(mat);
    return img;
}

}  // namespace

// -----------------------------------------------------------------------------
// Failure paths.
// -----------------------------------------------------------------------------

TEST_CASE("ImageProcessor: rejects invalid upscale requests",
          "[image_processor]") {
    ImageProcessor proc;

    SECTION("zero target dimensions") {
        auto src =
            make_image(cv::Mat(10, 10, CV_8UC3, cv::Scalar(128, 128, 128)));
        UpscaleOptions opts;
        opts.target_width = 0;
        opts.target_height = 0;
        REQUIRE(proc.upscale(*src, opts) == nullptr);
        REQUIRE_FALSE(proc.last_error().empty());
    }
    SECTION("empty source image") {
        Image empty;
        UpscaleOptions opts;
        opts.target_width = 100;
        opts.target_height = 100;
        REQUIRE(proc.upscale(empty, opts) == nullptr);
    }
    SECTION("upscale_to_match where src >= ref") {
        auto src =
            make_image(cv::Mat(64, 64, CV_8UC3, cv::Scalar(100, 100, 100)));
        auto ref =
            make_image(cv::Mat(32, 32, CV_8UC3, cv::Scalar(200, 200, 200)));
        REQUIRE(proc.upscale_to_match(*src, *ref) == nullptr);
    }
}

// -----------------------------------------------------------------------------
// upscale: dimensions, channel count and methods.
// -----------------------------------------------------------------------------

TEST_CASE("ImageProcessor: upscale resizes and preserves channel count "
          "across methods and pixel formats",
          "[image_processor]") {
    auto method = GENERATE(UpscaleMethod::Nearest, UpscaleMethod::Bilinear,
                           UpscaleMethod::Bicubic, UpscaleMethod::Lanczos);
    CAPTURE(static_cast<int>(method));

    struct Case {
        PixelFormat fmt;
        int cv_type;
        int channels;
    };
    auto c =
        GENERATE(Case{PixelFormat::RGB8, CV_8UC3, 3},
                 Case{PixelFormat::Gray8, CV_8UC1, 1});
    CAPTURE(static_cast<int>(c.fmt));

    cv::Mat src_mat(16, 16, c.cv_type, cv::Scalar::all(128));
    auto src = make_image(src_mat, c.fmt);

    ImageProcessor proc;
    UpscaleOptions opts;
    opts.target_width = 48;
    opts.target_height = 48;
    opts.method = method;

    auto r = proc.upscale(*src, opts);
    REQUIRE(r != nullptr);
    REQUIRE(r->info().width == 48);
    REQUIRE(r->info().height == 48);
    REQUIRE(r->mat().cols == 48);
    REQUIRE(r->mat().rows == 48);
    REQUIRE(r->mat().channels() == c.channels);
}

// -----------------------------------------------------------------------------
// upscale_to_match: smaller source matches reference dimensions.
// -----------------------------------------------------------------------------

TEST_CASE("ImageProcessor: upscale_to_match grows src to ref dimensions",
          "[image_processor]") {
    auto src = make_image(cv::Mat(32, 32, CV_8UC3, cv::Scalar(100, 100, 100)));
    auto ref = make_image(cv::Mat(64, 64, CV_8UC3, cv::Scalar(200, 200, 200)));

    ImageProcessor proc;
    auto r = proc.upscale_to_match(*src, *ref);
    REQUIRE(r != nullptr);
    REQUIRE(r->info().width == 64);
    REQUIRE(r->info().height == 64);
}
