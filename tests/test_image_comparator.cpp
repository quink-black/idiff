#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/image_comparator.h"
#include "core/image_impl.h"

#include <opencv2/core.hpp>

using namespace idiff;

// Helper: create an Image wrapping a cv::Mat
static std::unique_ptr<Image> make_image(cv::Mat mat, PixelFormat fmt = PixelFormat::RGB8) {
    auto img = std::make_unique<Image>();
    img->internal().info.width = mat.cols;
    img->internal().info.height = mat.rows;
    img->internal().info.pixel_format = fmt;
    img->internal().info.bit_depth = (fmt == PixelFormat::RGB16 || fmt == PixelFormat::Gray16 || fmt == PixelFormat::RGBA16) ? 16 : 8;
    img->internal().info.has_alpha = (fmt == PixelFormat::RGBA8 || fmt == PixelFormat::RGBA16);
    img->internal().mat = std::move(mat);
    return img;
}

TEST_CASE("ImageComparator: dimension mismatch returns nullptr", "[image_comparator]")
{
    auto a = make_image(cv::Mat::zeros(10, 10, CV_8UC3));
    auto b = make_image(cv::Mat::zeros(20, 20, CV_8UC3));

    ImageComparator cmp;
    auto diff = cmp.compute_difference(*a, *b);
    REQUIRE(diff == nullptr);
    REQUIRE_FALSE(cmp.last_error().empty());
}

TEST_CASE("ImageComparator: identical images produce zero diff", "[image_comparator]")
{
    cv::Mat data(64, 64, CV_8UC3, cv::Scalar(100, 150, 200));
    auto a = make_image(data.clone());
    auto b = make_image(data.clone());

    ImageComparator cmp;
    DifferenceOptions opts;
    opts.amplification = 1.0;

    auto diff = cmp.compute_difference(*a, *b, opts);
    REQUIRE(diff != nullptr);

    // All pixels should be zero
    double min_val, max_val;
    cv::minMaxLoc(diff->mat().reshape(1), &min_val, &max_val);
    REQUIRE(max_val == 0.0);
}

TEST_CASE("ImageComparator: different images produce nonzero diff", "[image_comparator]")
{
    auto a = make_image(cv::Mat(32, 32, CV_8UC3, cv::Scalar(100, 100, 100)));
    auto b = make_image(cv::Mat(32, 32, CV_8UC3, cv::Scalar(200, 200, 200)));

    ImageComparator cmp;
    DifferenceOptions opts;
    opts.amplification = 1.0;

    auto diff = cmp.compute_difference(*a, *b, opts);
    REQUIRE(diff != nullptr);

    double min_val, max_val;
    cv::minMaxLoc(diff->mat().reshape(1), &min_val, &max_val);
    REQUIRE(max_val == 100.0);
}

TEST_CASE("ImageComparator: amplification scales diff values", "[image_comparator]")
{
    auto a = make_image(cv::Mat(16, 16, CV_8UC3, cv::Scalar(100, 100, 100)));
    auto b = make_image(cv::Mat(16, 16, CV_8UC3, cv::Scalar(110, 110, 110)));

    ImageComparator cmp;

    DifferenceOptions opts1;
    opts1.amplification = 1.0;
    auto diff1 = cmp.compute_difference(*a, *b, opts1);

    DifferenceOptions opts5;
    opts5.amplification = 5.0;
    auto diff5 = cmp.compute_difference(*a, *b, opts5);

    REQUIRE(diff1 != nullptr);
    REQUIRE(diff5 != nullptr);

    double max1, max5;
    cv::minMaxLoc(diff1->mat().reshape(1), nullptr, &max1);
    cv::minMaxLoc(diff5->mat().reshape(1), nullptr, &max5);

    // 10 * 5 = 50, but clamped to 255 max for uint8
    REQUIRE(max1 == 10.0);
    REQUIRE(max5 == 50.0);
}

TEST_CASE("ImageComparator: threshold zeroes small differences", "[image_comparator]")
{
    auto a = make_image(cv::Mat(16, 16, CV_8UC3, cv::Scalar(100, 100, 100)));
    auto b = make_image(cv::Mat(16, 16, CV_8UC3, cv::Scalar(105, 105, 105)));

    ImageComparator cmp;
    DifferenceOptions opts;
    opts.amplification = 1.0;
    opts.threshold = 10;  // Threshold above the actual diff of 5

    auto diff = cmp.compute_difference(*a, *b, opts);
    REQUIRE(diff != nullptr);

    double max_val;
    cv::minMaxLoc(diff->mat().reshape(1), nullptr, &max_val);
    REQUIRE(max_val == 0.0);  // All below threshold
}

TEST_CASE("ImageComparator: heatmap from empty diff returns nullptr", "[image_comparator]")
{
    Image empty_img;
    ImageComparator cmp;
    auto heatmap = cmp.compute_heatmap(empty_img);
    REQUIRE(heatmap == nullptr);
    REQUIRE_FALSE(cmp.last_error().empty());
}

TEST_CASE("ImageComparator: heatmap produces 3-channel output", "[image_comparator]")
{
    auto a = make_image(cv::Mat(32, 32, CV_8UC3, cv::Scalar(50, 50, 50)));
    auto b = make_image(cv::Mat(32, 32, CV_8UC3, cv::Scalar(100, 100, 100)));

    ImageComparator cmp;
    DifferenceOptions opts;
    opts.amplification = 1.0;

    auto diff = cmp.compute_difference(*a, *b, opts);
    REQUIRE(diff != nullptr);

    auto heatmap = cmp.compute_heatmap(*diff, opts);
    REQUIRE(heatmap != nullptr);
    REQUIRE(heatmap->mat().channels() == 3);
    REQUIRE(heatmap->info().pixel_format == PixelFormat::RGB8);
}

TEST_CASE("ImageComparator: grayscale images can be compared", "[image_comparator]")
{
    auto a = make_image(cv::Mat(16, 16, CV_8UC1, cv::Scalar(100)), PixelFormat::Gray8);
    auto b = make_image(cv::Mat(16, 16, CV_8UC1, cv::Scalar(150)), PixelFormat::Gray8);

    ImageComparator cmp;
    DifferenceOptions opts;
    opts.amplification = 1.0;

    auto diff = cmp.compute_difference(*a, *b, opts);
    REQUIRE(diff != nullptr);
    // Output is normalized to RGB8
    REQUIRE(diff->mat().channels() == 3);
}
