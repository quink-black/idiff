#include <catch2/catch_test_macros.hpp>

#include "core/channel_view.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

using namespace idiff;

TEST_CASE("ChannelView: label for None is All", "[channel_view]") {
    REQUIRE(std::string(channel_view_mode_label(ChannelViewMode::None)) == "All");
}

TEST_CASE("ChannelView: label for AlphaGray", "[channel_view]") {
    REQUIRE(std::string(channel_view_mode_label(ChannelViewMode::AlphaGray)) == "Alpha (Gray)");
}

TEST_CASE("ChannelView: empty image returns nullopt", "[channel_view]") {
    cv::Mat empty;
    auto result = extract_channel_view(empty, ChannelViewMode::R);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("ChannelView: None returns clone of original", "[channel_view]") {
    cv::Mat rgb(4, 4, CV_8UC3, cv::Scalar(10, 20, 30));
    auto result = extract_channel_view(rgb, ChannelViewMode::None);
    REQUIRE(result.has_value());
    REQUIRE(result->rows == 4);
    REQUIRE(result->cols == 4);
    REQUIRE(result->channels() == 3);
    REQUIRE(result->at<cv::Vec3b>(0, 0) == cv::Vec3b(10, 20, 30));
}

TEST_CASE("ChannelView: R from RGB image", "[channel_view]") {
    cv::Mat rgb(2, 2, CV_8UC3);
    rgb.at<cv::Vec3b>(0, 0) = cv::Vec3b(10, 20, 30);
    rgb.at<cv::Vec3b>(0, 1) = cv::Vec3b(40, 50, 60);
    rgb.at<cv::Vec3b>(1, 0) = cv::Vec3b(70, 80, 90);
    rgb.at<cv::Vec3b>(1, 1) = cv::Vec3b(100, 110, 120);

    auto result = extract_channel_view(rgb, ChannelViewMode::R);
    REQUIRE(result.has_value());
    REQUIRE(result->channels() == 1);
    REQUIRE(result->at<uint8_t>(0, 0) == 10);
    REQUIRE(result->at<uint8_t>(0, 1) == 40);
    REQUIRE(result->at<uint8_t>(1, 0) == 70);
    REQUIRE(result->at<uint8_t>(1, 1) == 100);
}

TEST_CASE("ChannelView: G from RGB image", "[channel_view]") {
    cv::Mat rgb(2, 2, CV_8UC3);
    rgb.at<cv::Vec3b>(0, 0) = cv::Vec3b(10, 21, 30);

    auto result = extract_channel_view(rgb, ChannelViewMode::G);
    REQUIRE(result.has_value());
    REQUIRE(result->channels() == 1);
    REQUIRE(result->at<uint8_t>(0, 0) == 21);
}

TEST_CASE("ChannelView: B from RGB image", "[channel_view]") {
    cv::Mat rgb(2, 2, CV_8UC3);
    rgb.at<cv::Vec3b>(0, 0) = cv::Vec3b(10, 20, 32);

    auto result = extract_channel_view(rgb, ChannelViewMode::B);
    REQUIRE(result.has_value());
    REQUIRE(result->channels() == 1);
    REQUIRE(result->at<uint8_t>(0, 0) == 32);
}

TEST_CASE("ChannelView: RGB channel extraction fails on grayscale", "[channel_view]") {
    cv::Mat gray(2, 2, CV_8UC1, cv::Scalar(128));
    REQUIRE_FALSE(extract_channel_view(gray, ChannelViewMode::R).has_value());
    REQUIRE_FALSE(extract_channel_view(gray, ChannelViewMode::G).has_value());
    REQUIRE_FALSE(extract_channel_view(gray, ChannelViewMode::B).has_value());
}

TEST_CASE("ChannelView: AlphaGray from RGBA image", "[channel_view]") {
    cv::Mat rgba(2, 2, CV_8UC4);
    rgba.at<cv::Vec4b>(0, 0) = cv::Vec4b(10, 20, 30, 128);
    rgba.at<cv::Vec4b>(0, 1) = cv::Vec4b(40, 50, 60, 255);
    rgba.at<cv::Vec4b>(1, 0) = cv::Vec4b(70, 80, 90, 0);
    rgba.at<cv::Vec4b>(1, 1) = cv::Vec4b(100, 110, 120, 64);

    auto result = extract_channel_view(rgba, ChannelViewMode::AlphaGray);
    REQUIRE(result.has_value());
    REQUIRE(result->channels() == 1);
    REQUIRE(result->at<uint8_t>(0, 0) == 128);
    REQUIRE(result->at<uint8_t>(0, 1) == 255);
    REQUIRE(result->at<uint8_t>(1, 0) == 0);
    REQUIRE(result->at<uint8_t>(1, 1) == 64);
}

TEST_CASE("ChannelView: AlphaGray fails on RGB image", "[channel_view]") {
    cv::Mat rgb(2, 2, CV_8UC3, cv::Scalar(10, 20, 30));
    REQUIRE_FALSE(extract_channel_view(rgb, ChannelViewMode::AlphaGray).has_value());
}

TEST_CASE("ChannelView: AlphaBlend composites correctly", "[channel_view]") {
    cv::Mat rgba(2, 2, CV_8UC4);
    // Fully opaque pixel
    rgba.at<cv::Vec4b>(0, 0) = cv::Vec4b(200, 100, 50, 255);
    // Fully transparent pixel
    rgba.at<cv::Vec4b>(0, 1) = cv::Vec4b(0, 0, 0, 0);
    // 50% transparent
    rgba.at<cv::Vec4b>(1, 0) = cv::Vec4b(100, 100, 100, 128);
    rgba.at<cv::Vec4b>(1, 1) = cv::Vec4b(0, 0, 0, 255);

    auto result = extract_channel_view(rgba, ChannelViewMode::AlphaBlend);
    REQUIRE(result.has_value());
    REQUIRE(result->channels() == 4);
    REQUIRE(result->depth() == CV_8U);

    // Opaque pixel should remain unchanged
    cv::Vec4b p00 = result->at<cv::Vec4b>(0, 0);
    REQUIRE(p00[0] == 200);
    REQUIRE(p00[1] == 100);
    REQUIRE(p00[2] == 50);
    REQUIRE(p00[3] == 255);

    // Fully transparent pixel should show checkerboard background.
    // At (0,1) with 8x8 tiles, both coordinates are in the first tile
    // so the background is white (255).
    cv::Vec4b p01 = result->at<cv::Vec4b>(0, 1);
    REQUIRE(p01[0] == 255);
    REQUIRE(p01[1] == 255);
    REQUIRE(p01[2] == 255);
    REQUIRE(p01[3] == 255);

    // 50% transparent should blend toward the white background.
    // expected = (100 * 128 + 255 * 127) / 255 = ~177
    cv::Vec4b p10 = result->at<cv::Vec4b>(1, 0);
    REQUIRE(p10[0] > 120);
    REQUIRE(p10[0] < 200);
    REQUIRE(p10[3] == 255);
}

TEST_CASE("ChannelView: AlphaBlend fails on RGB image", "[channel_view]") {
    cv::Mat rgb(2, 2, CV_8UC3, cv::Scalar(10, 20, 30));
    REQUIRE_FALSE(extract_channel_view(rgb, ChannelViewMode::AlphaBlend).has_value());
}

TEST_CASE("ChannelView: Y from RGB image", "[channel_view]") {
    cv::Mat rgb(2, 2, CV_8UC3);
    rgb.at<cv::Vec3b>(0, 0) = cv::Vec3b(255, 0, 0);   // pure red
    rgb.at<cv::Vec3b>(0, 1) = cv::Vec3b(0, 255, 0);   // pure green
    rgb.at<cv::Vec3b>(1, 0) = cv::Vec3b(0, 0, 255);   // pure blue
    rgb.at<cv::Vec3b>(1, 1) = cv::Vec3b(255, 255, 255); // white

    auto result = extract_channel_view(rgb, ChannelViewMode::Y);
    REQUIRE(result.has_value());
    REQUIRE(result->channels() == 1);

    // Use OpenCV reference conversion to verify.
    cv::Mat yuv;
    cv::cvtColor(rgb, yuv, cv::COLOR_RGB2YUV);
    std::vector<cv::Mat> planes;
    cv::split(yuv, planes);

    REQUIRE(result->at<uint8_t>(0, 0) == planes[0].at<uint8_t>(0, 0));
    REQUIRE(result->at<uint8_t>(0, 1) == planes[0].at<uint8_t>(0, 1));
    REQUIRE(result->at<uint8_t>(1, 0) == planes[0].at<uint8_t>(1, 0));
    REQUIRE(result->at<uint8_t>(1, 1) == planes[0].at<uint8_t>(1, 1));
}

TEST_CASE("ChannelView: U and V from RGB image", "[channel_view]") {
    cv::Mat rgb(2, 2, CV_8UC3);
    rgb.at<cv::Vec3b>(0, 0) = cv::Vec3b(255, 128, 64);
    rgb.at<cv::Vec3b>(0, 1) = cv::Vec3b(10, 200, 30);

    cv::Mat yuv;
    cv::cvtColor(rgb, yuv, cv::COLOR_RGB2YUV);
    std::vector<cv::Mat> planes;
    cv::split(yuv, planes);

    auto u_result = extract_channel_view(rgb, ChannelViewMode::U);
    REQUIRE(u_result.has_value());
    REQUIRE(u_result->at<uint8_t>(0, 0) == planes[1].at<uint8_t>(0, 0));
    REQUIRE(u_result->at<uint8_t>(0, 1) == planes[1].at<uint8_t>(0, 1));

    auto v_result = extract_channel_view(rgb, ChannelViewMode::V);
    REQUIRE(v_result.has_value());
    REQUIRE(v_result->at<uint8_t>(0, 0) == planes[2].at<uint8_t>(0, 0));
    REQUIRE(v_result->at<uint8_t>(0, 1) == planes[2].at<uint8_t>(0, 1));
}

TEST_CASE("ChannelView: YUV extraction fails on grayscale", "[channel_view]") {
    cv::Mat gray(2, 2, CV_8UC1, cv::Scalar(128));
    REQUIRE_FALSE(extract_channel_view(gray, ChannelViewMode::Y).has_value());
    REQUIRE_FALSE(extract_channel_view(gray, ChannelViewMode::U).has_value());
    REQUIRE_FALSE(extract_channel_view(gray, ChannelViewMode::V).has_value());
}

TEST_CASE("ChannelView: YUV from RGBA drops alpha before conversion", "[channel_view]") {
    cv::Mat rgba(2, 2, CV_8UC4);
    rgba.at<cv::Vec4b>(0, 0) = cv::Vec4b(255, 128, 64, 128);

    auto result = extract_channel_view(rgba, ChannelViewMode::Y);
    REQUIRE(result.has_value());
    REQUIRE(result->channels() == 1);

    // Verify by converting the RGB portion directly.
    cv::Mat rgb;
    cv::cvtColor(rgba, rgb, cv::COLOR_RGBA2RGB);
    cv::Mat yuv;
    cv::cvtColor(rgb, yuv, cv::COLOR_RGB2YUV);
    std::vector<cv::Mat> planes;
    cv::split(yuv, planes);

    REQUIRE(result->at<uint8_t>(0, 0) == planes[0].at<uint8_t>(0, 0));
}

TEST_CASE("ChannelView: 16-bit RGB preserves depth for R/G/B", "[channel_view]") {
    cv::Mat rgb16(2, 2, CV_16UC3);
    rgb16.at<cv::Vec3w>(0, 0) = cv::Vec3w(1000, 2000, 3000);

    auto r = extract_channel_view(rgb16, ChannelViewMode::R);
    REQUIRE(r.has_value());
    REQUIRE(r->depth() == CV_16U);
    REQUIRE(r->at<uint16_t>(0, 0) == 1000);

    auto g = extract_channel_view(rgb16, ChannelViewMode::G);
    REQUIRE(g.has_value());
    REQUIRE(g->depth() == CV_16U);
    REQUIRE(g->at<uint16_t>(0, 0) == 2000);

    auto b = extract_channel_view(rgb16, ChannelViewMode::B);
    REQUIRE(b.has_value());
    REQUIRE(b->depth() == CV_16U);
    REQUIRE(b->at<uint16_t>(0, 0) == 3000);
}

TEST_CASE("ChannelView: 16-bit AlphaGray preserves depth", "[channel_view]") {
    cv::Mat rgba16(2, 2, CV_16UC4);
    rgba16.at<cv::Vec4w>(0, 0) = cv::Vec4w(1000, 2000, 3000, 4000);

    auto result = extract_channel_view(rgba16, ChannelViewMode::AlphaGray);
    REQUIRE(result.has_value());
    REQUIRE(result->depth() == CV_16U);
    REQUIRE(result->at<uint16_t>(0, 0) == 4000);
}

TEST_CASE("ChannelView: 16-bit YUV downsampled to 8-bit", "[channel_view]") {
    cv::Mat rgb16(2, 2, CV_16UC3);
    rgb16.at<cv::Vec3w>(0, 0) = cv::Vec3w(65535, 32768, 0);

    auto result = extract_channel_view(rgb16, ChannelViewMode::Y);
    REQUIRE(result.has_value());
    // OpenCV does not support 16-bit YUV conversion, so we downsample.
    REQUIRE(result->depth() == CV_8U);
}

TEST_CASE("ChannelView: AlphaContour label", "[channel_view]") {
    REQUIRE(std::string(channel_view_mode_label(ChannelViewMode::AlphaContour)) == "Alpha Contour");
}

TEST_CASE("ChannelView: AlphaContour fails on RGB image", "[channel_view]") {
    cv::Mat rgb(2, 2, CV_8UC3, cv::Scalar(10, 20, 30));
    REQUIRE_FALSE(extract_channel_view(rgb, ChannelViewMode::AlphaContour).has_value());
}

TEST_CASE("ChannelView: AlphaContour produces RGBA8 output", "[channel_view]") {
    // Build a small RGBA image with a sharp alpha boundary in the middle.
    cv::Mat rgba(8, 8, CV_8UC4, cv::Vec4b(100, 150, 200, 255));
    // Left half opaque, right half transparent.
    for (int y = 0; y < 8; ++y) {
        for (int x = 4; x < 8; ++x) {
            rgba.at<cv::Vec4b>(y, x) = cv::Vec4b(100, 150, 200, 0);
        }
    }

    auto result = extract_channel_view(rgba, ChannelViewMode::AlphaContour);
    REQUIRE(result.has_value());
    REQUIRE(result->channels() == 4);
    REQUIRE(result->depth() == CV_8U);

    // At column 3 (just left of the alpha boundary) the Canny edge should
    // produce red contour pixels.  At column 0 (far from edge) no contour.
    bool found_red = false;
    for (int y = 0; y < 8; ++y) {
        cv::Vec4b p = result->at<cv::Vec4b>(y, 3);
        if (p[0] == 255 && p[1] == 0 && p[2] == 0 && p[3] == 255) {
            found_red = true;
            break;
        }
    }
    REQUIRE(found_red);

    // Far from the edge (col 0) should be dimmed RGB, no red.
    cv::Vec4b p0 = result->at<cv::Vec4b>(0, 0);
    REQUIRE(p0[0] < 100);  // dimmed from 100
    REQUIRE(p0[1] > 0);
    REQUIRE(p0[2] > 0);
    REQUIRE(p0[3] == 255);
}
