// JSON produced for metrics.compare and pixel.sample: metric values and
// the identical-image case, per-image errors, and sampled pixel values
// at 8-bit and 16-bit depth.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "app/rpc_queries.h"
#include "core/image_impl.h"

#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>

#include <memory>

using namespace idiff;
using Catch::Matchers::WithinAbs;
using nlohmann::json;

namespace {

std::unique_ptr<Image> make_image(cv::Mat mat,
                                  PixelFormat fmt = PixelFormat::RGB8) {
    auto img = std::make_unique<Image>();
    img->internal().info.width = mat.cols;
    img->internal().info.height = mat.rows;
    img->internal().info.pixel_format = fmt;
    img->internal().info.bit_depth = fmt == PixelFormat::RGB16 ? 16 : 8;
    img->internal().mat = std::move(mat);
    return img;
}

} // namespace

TEST_CASE("compare_images reports identical images with a null PSNR",
          "[rpc][queries]") {
    auto a = make_image(cv::Mat(16, 16, CV_8UC3, cv::Scalar(10, 20, 30)));
    auto b = make_image(cv::Mat(16, 16, CV_8UC3, cv::Scalar(10, 20, 30)));
    json r = rpc_queries::compare_images(*a, *b);
    REQUIRE(r["identical"] == true);
    REQUIRE(r["psnr"].is_null());
    REQUIRE(r["mse"].get<double>() == 0.0);
    REQUIRE_THAT(r["ssim"].get<double>(), WithinAbs(1.0, 1e-9));
    REQUIRE_FALSE(r.contains("error"));
}

TEST_CASE("compare_images reports MSE and PSNR of differing images",
          "[rpc][queries]") {
    auto a = make_image(cv::Mat(16, 16, CV_8UC3, cv::Scalar(100, 100, 100)));
    auto b = make_image(cv::Mat(16, 16, CV_8UC3, cv::Scalar(110, 110, 110)));
    json r = rpc_queries::compare_images(*a, *b);
    REQUIRE(r["identical"] == false);
    REQUIRE_THAT(r["mse"].get<double>(), WithinAbs(100.0, 1e-9));
    REQUIRE_THAT(r["psnr"].get<double>(),
                 WithinAbs(10.0 * std::log10(255.0 * 255.0 / 100.0), 1e-9));
}

TEST_CASE("compare_images reports a size mismatch as an error",
          "[rpc][queries]") {
    auto a = make_image(cv::Mat(16, 16, CV_8UC3, cv::Scalar(0, 0, 0)));
    auto b = make_image(cv::Mat(8, 8, CV_8UC3, cv::Scalar(0, 0, 0)));
    json r = rpc_queries::compare_images(*a, *b);
    REQUIRE(r.contains("error"));
    REQUIRE_FALSE(r.contains("psnr"));
}

TEST_CASE("sample_pixel reads an 8-bit RGB pixel", "[rpc][queries]") {
    cv::Mat m(4, 6, CV_8UC3, cv::Scalar(0, 0, 0));
    m.at<cv::Vec3b>(2, 5) = cv::Vec3b(1, 2, 3);
    auto img = make_image(m);
    json r = rpc_queries::sample_pixel(*img, 5, 2);
    REQUIRE(r["channels"] == 3);
    REQUIRE(r["depth"] == "8u");
    REQUIRE(r["values"] == json::array({1, 2, 3}));
    REQUIRE(r["text"].is_string());
    REQUIRE_FALSE(r["text"].get<std::string>().empty());
}

TEST_CASE("sample_pixel keeps 16-bit values", "[rpc][queries]") {
    cv::Mat m(2, 2, CV_16UC3, cv::Scalar(0, 0, 0));
    m.at<cv::Vec3w>(1, 0) = cv::Vec3w(1023, 512, 65535);
    auto img = make_image(m, PixelFormat::RGB16);
    json r = rpc_queries::sample_pixel(*img, 0, 1);
    REQUIRE(r["depth"] == "16u");
    REQUIRE(r["values"] == json::array({1023, 512, 65535}));
}

TEST_CASE("sample_pixel reports a coordinate outside the image",
          "[rpc][queries]") {
    auto img = make_image(cv::Mat(4, 6, CV_8UC3, cv::Scalar(0, 0, 0)));
    REQUIRE(rpc_queries::sample_pixel(*img, 6, 0).contains("error"));
    REQUIRE(rpc_queries::sample_pixel(*img, 0, 4).contains("error"));
    REQUIRE(rpc_queries::sample_pixel(*img, -1, 0).contains("error"));
}
