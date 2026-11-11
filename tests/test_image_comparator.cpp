#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "core/image_comparator.h"
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

double max_value(const cv::Mat& m) {
    double v;
    cv::minMaxLoc(m.reshape(1), nullptr, &v);
    return v;
}

}  // namespace

// -----------------------------------------------------------------------------
// Failure paths.
// -----------------------------------------------------------------------------

TEST_CASE("ImageComparator: surfaces failures via last_error",
          "[image_comparator]") {
    ImageComparator cmp;

    SECTION("dimension mismatch") {
        auto a = make_image(cv::Mat::zeros(10, 10, CV_8UC3));
        auto b = make_image(cv::Mat::zeros(20, 20, CV_8UC3));
        REQUIRE(cmp.compute_difference(*a, *b) == nullptr);
        REQUIRE_FALSE(cmp.last_error().empty());
    }

    SECTION("heatmap of an empty diff") {
        Image empty;
        REQUIRE(cmp.compute_heatmap(empty) == nullptr);
        REQUIRE_FALSE(cmp.last_error().empty());
    }
}

// -----------------------------------------------------------------------------
// Difference behavior across pixel formats.
// -----------------------------------------------------------------------------

TEST_CASE("ImageComparator: identical images produce a zero RGB8 diff",
          "[image_comparator]") {
    struct Case {
        PixelFormat fmt;
        int cv_type;
        cv::Scalar value;
    };
    auto c = GENERATE(
        Case{PixelFormat::RGB8, CV_8UC3, cv::Scalar(100, 150, 200)},
        Case{PixelFormat::RGB16, CV_16UC3, cv::Scalar(40000, 50000, 60000)});
    CAPTURE(static_cast<int>(c.fmt));

    cv::Mat data(32, 32, c.cv_type, c.value);
    auto a = make_image(data.clone(), c.fmt);
    auto b = make_image(data.clone(), c.fmt);

    ImageComparator cmp;
    DifferenceOptions opts;
    opts.amplification = 1.0;
    auto diff = cmp.compute_difference(*a, *b, opts);
    REQUIRE(diff != nullptr);
    REQUIRE(diff->mat().depth() == CV_8U);
    REQUIRE(diff->mat().channels() == 3);
    REQUIRE(max_value(diff->mat()) == 0.0);
}

TEST_CASE("ImageComparator: different images produce a nonzero RGB8 diff",
          "[image_comparator]") {
    struct Case {
        PixelFormat fmt;
        int cv_type;
        cv::Scalar a_value;
        cv::Scalar b_value;
        double expected_max;  // 0 means just > 0
    };
    auto c = GENERATE(
        Case{PixelFormat::RGB8, CV_8UC3, cv::Scalar(100, 100, 100),
             cv::Scalar(200, 200, 200), 100.0},
        Case{PixelFormat::Gray8, CV_8UC1, cv::Scalar(100), cv::Scalar(150),
             0.0},
        Case{PixelFormat::RGB16, CV_16UC3, cv::Scalar(10000, 10000, 10000),
             cv::Scalar(30000, 30000, 30000), 0.0},
        Case{PixelFormat::Gray16, CV_16UC1, cv::Scalar(20000),
             cv::Scalar(40000), 0.0},
        Case{PixelFormat::RGBA16, CV_16UC4,
             cv::Scalar(10000, 20000, 30000, 65535),
             cv::Scalar(30000, 40000, 50000, 65535), 0.0});
    CAPTURE(static_cast<int>(c.fmt));

    auto a = make_image(cv::Mat(32, 32, c.cv_type, c.a_value), c.fmt);
    auto b = make_image(cv::Mat(32, 32, c.cv_type, c.b_value), c.fmt);

    ImageComparator cmp;
    DifferenceOptions opts;
    opts.amplification = 1.0;
    auto diff = cmp.compute_difference(*a, *b, opts);
    REQUIRE(diff != nullptr);
    // Diff output is always RGB8 regardless of input format.
    REQUIRE(diff->info().pixel_format == PixelFormat::RGB8);
    REQUIRE(diff->mat().depth() == CV_8U);
    REQUIRE(diff->mat().channels() == 3);

    if (c.expected_max > 0.0) {
        REQUIRE(max_value(diff->mat()) == c.expected_max);
    } else {
        REQUIRE(max_value(diff->mat()) > 0.0);
    }
}

// -----------------------------------------------------------------------------
// Diff option knobs.
// -----------------------------------------------------------------------------

TEST_CASE("ImageComparator: amplification scales diff values",
          "[image_comparator]") {
    auto a =
        make_image(cv::Mat(16, 16, CV_8UC3, cv::Scalar(100, 100, 100)));
    auto b =
        make_image(cv::Mat(16, 16, CV_8UC3, cv::Scalar(110, 110, 110)));

    ImageComparator cmp;

    DifferenceOptions o1;
    o1.amplification = 1.0;
    auto d1 = cmp.compute_difference(*a, *b, o1);

    DifferenceOptions o5;
    o5.amplification = 5.0;
    auto d5 = cmp.compute_difference(*a, *b, o5);

    REQUIRE(d1 != nullptr);
    REQUIRE(d5 != nullptr);
    REQUIRE(max_value(d1->mat()) == 10.0);
    REQUIRE(max_value(d5->mat()) == 50.0);
}

TEST_CASE("ImageComparator: threshold zeroes small differences",
          "[image_comparator]") {
    auto a =
        make_image(cv::Mat(16, 16, CV_8UC3, cv::Scalar(100, 100, 100)));
    auto b =
        make_image(cv::Mat(16, 16, CV_8UC3, cv::Scalar(105, 105, 105)));

    DifferenceOptions opts;
    opts.amplification = 1.0;
    opts.threshold = 10;

    ImageComparator cmp;
    auto diff = cmp.compute_difference(*a, *b, opts);
    REQUIRE(diff != nullptr);
    REQUIRE(max_value(diff->mat()) == 0.0);
}

// -----------------------------------------------------------------------------
// Heatmap.
// -----------------------------------------------------------------------------

TEST_CASE("ImageComparator: heatmap produces 3-channel RGB8 output",
          "[image_comparator]") {
    auto a = make_image(cv::Mat(32, 32, CV_8UC3, cv::Scalar(50, 50, 50)));
    auto b =
        make_image(cv::Mat(32, 32, CV_8UC3, cv::Scalar(100, 100, 100)));

    ImageComparator cmp;
    auto diff = cmp.compute_difference(*a, *b);
    REQUIRE(diff != nullptr);

    auto heatmap = cmp.compute_heatmap(*diff);
    REQUIRE(heatmap != nullptr);
    REQUIRE(heatmap->mat().channels() == 3);
    REQUIRE(heatmap->info().pixel_format == PixelFormat::RGB8);
}

TEST_CASE("ImageComparator: grayscale images can be compared", "[image_comparator]"){
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

TEST_CASE("ImageComparator: 16-bit identical images produce zero diff", "[image_comparator]")
{
    cv::Mat data(32, 32, CV_16UC3, cv::Scalar(40000, 50000, 60000));
    auto a = make_image(data.clone(), PixelFormat::RGB16);
    auto b = make_image(data.clone(), PixelFormat::RGB16);

    ImageComparator cmp;
    DifferenceOptions opts;
    opts.amplification = 1.0;

    auto diff = cmp.compute_difference(*a, *b, opts);
    REQUIRE(diff != nullptr);

    double max_val;
    cv::minMaxLoc(diff->mat().reshape(1), nullptr, &max_val);
    REQUIRE(max_val == 0.0);
}

TEST_CASE("ImageComparator: 16-bit different images produce nonzero diff", "[image_comparator]")
{
    auto a = make_image(cv::Mat(32, 32, CV_16UC3, cv::Scalar(10000, 10000, 10000)), PixelFormat::RGB16);
    auto b = make_image(cv::Mat(32, 32, CV_16UC3, cv::Scalar(30000, 30000, 30000)), PixelFormat::RGB16);

    ImageComparator cmp;
    DifferenceOptions opts;
    opts.amplification = 1.0;

    auto diff = cmp.compute_difference(*a, *b, opts);
    REQUIRE(diff != nullptr);

    double max_val;
    cv::minMaxLoc(diff->mat().reshape(1), nullptr, &max_val);
    REQUIRE(max_val > 0.0);
    // Output is always 8-bit RGB
    REQUIRE(diff->mat().depth() == CV_8U);
    REQUIRE(diff->mat().channels() == 3);
}

TEST_CASE("ImageComparator: Gray16 images can be compared", "[image_comparator]")
{
    auto a = make_image(cv::Mat(16, 16, CV_16UC1, cv::Scalar(20000)), PixelFormat::Gray16);
    auto b = make_image(cv::Mat(16, 16, CV_16UC1, cv::Scalar(40000)), PixelFormat::Gray16);

    ImageComparator cmp;
    DifferenceOptions opts;
    opts.amplification = 1.0;

    auto diff = cmp.compute_difference(*a, *b, opts);
    REQUIRE(diff != nullptr);
    REQUIRE(diff->mat().channels() == 3);
    REQUIRE(diff->mat().depth() == CV_8U);

    double max_val;
    cv::minMaxLoc(diff->mat().reshape(1), nullptr, &max_val);
    REQUIRE(max_val > 0.0);
}

TEST_CASE("ImageComparator: RGBA16 images can be compared", "[image_comparator]")
{
    auto a = make_image(cv::Mat(16, 16, CV_16UC4, cv::Scalar(10000, 20000, 30000, 65535)), PixelFormat::RGBA16);
    auto b = make_image(cv::Mat(16, 16, CV_16UC4, cv::Scalar(30000, 40000, 50000, 65535)), PixelFormat::RGBA16);

    ImageComparator cmp;
    auto diff = cmp.compute_difference(*a, *b);
    REQUIRE(diff != nullptr);
    REQUIRE(diff->info().pixel_format == PixelFormat::RGB8);
}

// -----------------------------------------------------------------------------
// Single-image reveal.
// -----------------------------------------------------------------------------

namespace {

// Flat field with a centered square whose luminance is offset by
// `delta`.  Returns the base image and leaves the square's bounds in
// `patch`.
std::unique_ptr<Image> make_offset_square(int size, int patch_size,
                                          int base, int delta,
                                          cv::Rect& patch) {
    cv::Mat m(size, size, CV_8UC3, cv::Scalar(base, base, base));
    const int p = (size - patch_size) / 2;
    patch = cv::Rect(p, p, patch_size, patch_size);
    m(patch).setTo(cv::Scalar(base + delta, base + delta, base + delta));
    return make_image(std::move(m));
}

double mean_of(const cv::Mat& m, const cv::Rect& roi) {
    return cv::mean(m(roi))[0];
}

}  // namespace

TEST_CASE("ImageComparator: reveal rejects an empty image", "[reveal]") {
    ImageComparator cmp;
    Image empty;
    REQUIRE(cmp.compute_reveal(empty) == nullptr);
    REQUIRE_FALSE(cmp.last_error().empty());
}

TEST_CASE("ImageComparator: reveal of a flat image is uniform mid-gray",
          "[reveal]") {
    auto src = make_image(cv::Mat(64, 64, CV_8UC3, cv::Scalar(120, 120, 120)));

    ImageComparator cmp;
    RevealOptions opts;
    opts.heatmap_color = HeatmapColor::Gray;
    auto out = cmp.compute_reveal(*src, opts);
    REQUIRE(out != nullptr);
    REQUIRE(out->mat().cols == 64);
    REQUIRE(out->mat().rows == 64);
    REQUIRE(out->mat().depth() == CV_8U);

    double lo, hi;
    cv::minMaxLoc(out->mat(), &lo, &hi);
    REQUIRE(lo == 128.0);
    REQUIRE(hi == 128.0);
}

TEST_CASE("ImageComparator: reveal makes a faint bright patch stand out",
          "[reveal]") {
    // A patch wider than the background sigma survives the subtraction
    // as a signed offset; the sign tells which way it deviates.
    cv::Rect patch;
    auto src = make_offset_square(128, 40, 100, 3, patch);

    ImageComparator cmp;
    RevealOptions opts;
    opts.background_sigma = 60.0;
    opts.amplification = 20.0;
    opts.smooth = false;
    opts.heatmap_color = HeatmapColor::Gray;

    auto out = cmp.compute_reveal(*src, opts);
    REQUIRE(out != nullptr);

    const cv::Rect center(patch.x + 12, patch.y + 12, 16, 16);
    const cv::Rect outside(2, 2, 16, 16);
    REQUIRE(mean_of(out->mat(), center) > 140.0);
    REQUIRE(mean_of(out->mat(), outside) < 130.0);
}

TEST_CASE("ImageComparator: reveal keeps the sign of a dark patch",
          "[reveal]") {
    cv::Rect patch;
    auto src = make_offset_square(128, 40, 180, -3, patch);

    ImageComparator cmp;
    RevealOptions opts;
    opts.background_sigma = 60.0;
    opts.smooth = false;
    opts.heatmap_color = HeatmapColor::Gray;

    auto out = cmp.compute_reveal(*src, opts);
    REQUIRE(out != nullptr);

    const cv::Rect center(patch.x + 12, patch.y + 12, 16, 16);
    REQUIRE(mean_of(out->mat(), center) < 120.0);
}

TEST_CASE("ImageComparator: reveal honors the requested palette",
          "[reveal]") {
    cv::Rect patch;
    auto src = make_offset_square(64, 32, 100, 5, patch);

    ImageComparator cmp;
    RevealOptions opts;
    opts.background_sigma = 40.0;
    opts.heatmap_color = HeatmapColor::Coolwarm;

    auto out = cmp.compute_reveal(*src, opts);
    REQUIRE(out != nullptr);
    REQUIRE(out->mat().channels() == 3);
    REQUIRE(out->info().pixel_format == PixelFormat::RGB8);
}

TEST_CASE("ImageComparator: reveal survives a radius larger than a tiny image",
          "[reveal]") {
    // Regression: the background estimate is computed on a reduced
    // copy, and an unclamped reduction factor asked cv::resize for a
    // zero-sized image.
    cv::Rect patch;
    auto src = make_offset_square(9, 5, 128, 10, patch);

    ImageComparator cmp;
    RevealOptions opts;
    opts.background_sigma = 1000.0;
    auto out = cmp.compute_reveal(*src, opts);
    REQUIRE(out != nullptr);
    REQUIRE(out->mat().cols == 9);
    REQUIRE(out->mat().rows == 9);
}

TEST_CASE("ImageComparator: reveal keeps sub-8-bit offsets from 16-bit input",
          "[reveal]") {
    // One 16-bit step is 1/257 of an 8-bit step.  Quantizing before
    // the subtraction would erase this patch entirely.
    constexpr int kBase = 100 * 257;
    cv::Mat m(128, 128, CV_16UC3, cv::Scalar(kBase, kBase, kBase));
    const cv::Rect patch(44, 44, 40, 40);
    m(patch).setTo(cv::Scalar(kBase + 1, kBase + 1, kBase + 1));
    auto src = make_image(std::move(m), PixelFormat::RGB16);

    ImageComparator cmp;
    RevealOptions opts;
    opts.background_sigma = 60.0;
    opts.amplification = 200.0;
    opts.smooth = false;
    opts.heatmap_color = HeatmapColor::Gray;

    auto out = cmp.compute_reveal(*src, opts);
    REQUIRE(out != nullptr);

    const cv::Rect center(patch.x + 12, patch.y + 12, 16, 16);
    const cv::Rect outside(2, 2, 16, 16);
    REQUIRE(mean_of(out->mat(), center) > 128.0);
    REQUIRE(mean_of(out->mat(), outside) == 128.0);
}
