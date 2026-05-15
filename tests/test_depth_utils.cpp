#include <catch2/catch_test_macros.hpp>

#include "core/depth_utils.h"

#include <opencv2/core.hpp>

using namespace idiff;

// -- convert_to_8u ---------------------------------------------------------

TEST_CASE("convert_to_8u: empty input returns empty", "[depth_utils]") {
    cv::Mat empty;
    cv::Mat result = convert_to_8u(empty);
    REQUIRE(result.empty());
}

TEST_CASE("convert_to_8u: CV_8U pass-through", "[depth_utils]") {
    cv::Mat src(2, 2, CV_8UC3, cv::Scalar(100, 150, 200));
    cv::Mat result = convert_to_8u(src);
    REQUIRE(result.depth() == CV_8U);
    REQUIRE(result.channels() == 3);
    REQUIRE(result.data == src.data);
}

TEST_CASE("convert_to_8u: CV_8UC1 pass-through", "[depth_utils]") {
    cv::Mat src(2, 2, CV_8UC1, cv::Scalar(42));
    cv::Mat result = convert_to_8u(src);
    REQUIRE(result.depth() == CV_8U);
    REQUIRE(result.channels() == 1);
    REQUIRE(result.at<uint8_t>(0, 0) == 42);
}

TEST_CASE("convert_to_8u: CV_16UC3 scales correctly", "[depth_utils]") {
    // 65535 -> 255, 32896 (128*257) -> 128, 0 -> 0
    cv::Mat src(1, 3, CV_16UC3);
    src.at<cv::Vec3w>(0, 0) = cv::Vec3w(65535, 0, 32896);
    src.at<cv::Vec3w>(0, 1) = cv::Vec3w(0, 65535, 0);
    src.at<cv::Vec3w>(0, 2) = cv::Vec3w(32896, 32896, 65535);

    cv::Mat result = convert_to_8u(src);
    REQUIRE(result.depth() == CV_8U);
    REQUIRE(result.channels() == 3);
    REQUIRE(result.at<cv::Vec3b>(0, 0)[0] == 255);
    REQUIRE(result.at<cv::Vec3b>(0, 0)[1] == 0);
    REQUIRE(result.at<cv::Vec3b>(0, 0)[2] == 128);
}

TEST_CASE("convert_to_8u: CV_16UC1 gray", "[depth_utils]") {
    cv::Mat src(2, 2, CV_16UC1, cv::Scalar(65535));
    cv::Mat result = convert_to_8u(src);
    REQUIRE(result.depth() == CV_8U);
    REQUIRE(result.channels() == 1);
    REQUIRE(result.at<uint8_t>(0, 0) == 255);
}

TEST_CASE("convert_to_8u: CV_16UC4 rgba", "[depth_utils]") {
    cv::Mat src(1, 1, CV_16UC4, cv::Scalar(65535, 0, 32896, 65535));
    cv::Mat result = convert_to_8u(src);
    REQUIRE(result.depth() == CV_8U);
    REQUIRE(result.channels() == 4);
    REQUIRE(result.at<cv::Vec4b>(0, 0)[0] == 255);
    REQUIRE(result.at<cv::Vec4b>(0, 0)[1] == 0);
    REQUIRE(result.at<cv::Vec4b>(0, 0)[2] == 128);
    REQUIRE(result.at<cv::Vec4b>(0, 0)[3] == 255);
}

TEST_CASE("convert_to_8u: CV_16SC3 signed to unsigned then scales", "[depth_utils]") {
    // CV_16S range is [-32768, 32767]. After shifting by +32768:
    //   -32768 -> 0     -> 0
    //    32767 -> 65535  -> 255
    //        0 -> 32768  -> ~128
    cv::Mat src(1, 1, CV_16SC3, cv::Scalar(-32768, 32767, 0));
    cv::Mat result = convert_to_8u(src);
    REQUIRE(result.depth() == CV_8U);
    REQUIRE(result.channels() == 3);
    REQUIRE(result.at<cv::Vec3b>(0, 0)[0] == 0);
    REQUIRE(result.at<cv::Vec3b>(0, 0)[1] == 255);
    REQUIRE(result.at<cv::Vec3b>(0, 0)[2] == 128);
}

TEST_CASE("convert_to_8u: CV_16SC1 gray signed", "[depth_utils]") {
    cv::Mat src(2, 2, CV_16SC1, cv::Scalar(32767));
    cv::Mat result = convert_to_8u(src);
    REQUIRE(result.depth() == CV_8U);
    REQUIRE(result.channels() == 1);
    REQUIRE(result.at<uint8_t>(0, 0) == 255);
}

TEST_CASE("convert_to_rgba8: CV_16SC3 -> RGBA8", "[depth_utils]") {
    cv::Mat src(1, 1, CV_16SC3, cv::Scalar(32767, -32768, 0));
    cv::Mat result = convert_to_rgba8(src);
    REQUIRE(result.depth() == CV_8U);
    REQUIRE(result.channels() == 4);
    REQUIRE(result.at<cv::Vec4b>(0, 0)[0] == 255);
    REQUIRE(result.at<cv::Vec4b>(0, 0)[1] == 0);
    REQUIRE(result.at<cv::Vec4b>(0, 0)[3] == 255);
}

TEST_CASE("convert_to_8u: CV_32FC3 maps [0,1] to [0,255]", "[depth_utils]") {
    cv::Mat src(1, 1, CV_32FC3, cv::Scalar(1.0f, 0.0f, 0.5f));
    cv::Mat result = convert_to_8u(src);
    REQUIRE(result.depth() == CV_8U);
    REQUIRE(result.channels() == 3);
    REQUIRE(result.at<cv::Vec3b>(0, 0)[0] == 255);
    REQUIRE(result.at<cv::Vec3b>(0, 0)[1] == 0);
    // 0.5 * 255 = 127.5 -> 128 (rounding)
    REQUIRE(result.at<cv::Vec3b>(0, 0)[2] >= 127);
    REQUIRE(result.at<cv::Vec3b>(0, 0)[2] <= 128);
}

TEST_CASE("convert_to_8u: CV_32FC1 gray", "[depth_utils]") {
    cv::Mat src(2, 2, CV_32FC1, cv::Scalar(0.75f));
    cv::Mat result = convert_to_8u(src);
    REQUIRE(result.depth() == CV_8U);
    REQUIRE(result.channels() == 1);
    // 0.75 * 255 = 191.25
    REQUIRE(result.at<uint8_t>(0, 0) >= 191);
    REQUIRE(result.at<uint8_t>(0, 0) <= 192);
}

TEST_CASE("convert_to_8u: CV_32F clamps above 1.0", "[depth_utils]") {
    cv::Mat src(1, 1, CV_32FC3, cv::Scalar(2.5f, -0.5f, 0.5f));
    cv::Mat result = convert_to_8u(src);
    REQUIRE(result.at<cv::Vec3b>(0, 0)[0] == 255);
    REQUIRE(result.at<cv::Vec3b>(0, 0)[1] == 0);
}

// -- convert_to_rgba8 -------------------------------------------------------

TEST_CASE("convert_to_rgba8: empty input returns empty", "[depth_utils]") {
    cv::Mat empty;
    cv::Mat result = convert_to_rgba8(empty);
    REQUIRE(result.empty());
}

TEST_CASE("convert_to_rgba8: CV_8UC1 -> RGBA8", "[depth_utils]") {
    cv::Mat src(2, 2, CV_8UC1, cv::Scalar(128));
    cv::Mat result = convert_to_rgba8(src);
    REQUIRE(result.depth() == CV_8U);
    REQUIRE(result.channels() == 4);
    REQUIRE(result.at<cv::Vec4b>(0, 0)[0] == 128);
    REQUIRE(result.at<cv::Vec4b>(0, 0)[1] == 128);
    REQUIRE(result.at<cv::Vec4b>(0, 0)[2] == 128);
    REQUIRE(result.at<cv::Vec4b>(0, 0)[3] == 255);
}

TEST_CASE("convert_to_rgba8: CV_8UC3 -> RGBA8", "[depth_utils]") {
    cv::Mat src(2, 2, CV_8UC3, cv::Scalar(10, 20, 30));
    cv::Mat result = convert_to_rgba8(src);
    REQUIRE(result.depth() == CV_8U);
    REQUIRE(result.channels() == 4);
    REQUIRE(result.at<cv::Vec4b>(0, 0)[0] == 10);
    REQUIRE(result.at<cv::Vec4b>(0, 0)[1] == 20);
    REQUIRE(result.at<cv::Vec4b>(0, 0)[2] == 30);
    REQUIRE(result.at<cv::Vec4b>(0, 0)[3] == 255);
}

TEST_CASE("convert_to_rgba8: CV_8UC4 pass-through", "[depth_utils]") {
    cv::Mat src(2, 2, CV_8UC4, cv::Scalar(10, 20, 30, 128));
    cv::Mat result = convert_to_rgba8(src);
    REQUIRE(result.depth() == CV_8U);
    REQUIRE(result.channels() == 4);
    REQUIRE(result.at<cv::Vec4b>(0, 0) == cv::Vec4b(10, 20, 30, 128));
}

TEST_CASE("convert_to_rgba8: CV_16UC3 -> RGBA8", "[depth_utils]") {
    cv::Mat src(1, 1, CV_16UC3, cv::Scalar(65535, 0, 32896));
    cv::Mat result = convert_to_rgba8(src);
    REQUIRE(result.depth() == CV_8U);
    REQUIRE(result.channels() == 4);
    REQUIRE(result.at<cv::Vec4b>(0, 0)[0] == 255);
    REQUIRE(result.at<cv::Vec4b>(0, 0)[1] == 0);
    REQUIRE(result.at<cv::Vec4b>(0, 0)[2] == 128);
    REQUIRE(result.at<cv::Vec4b>(0, 0)[3] == 255);
}

TEST_CASE("convert_to_rgba8: CV_16UC1 gray -> RGBA8", "[depth_utils]") {
    cv::Mat src(1, 1, CV_16UC1, cv::Scalar(65535));
    cv::Mat result = convert_to_rgba8(src);
    REQUIRE(result.depth() == CV_8U);
    REQUIRE(result.channels() == 4);
    REQUIRE(result.at<cv::Vec4b>(0, 0)[0] == 255);
    REQUIRE(result.at<cv::Vec4b>(0, 0)[3] == 255);
}

TEST_CASE("convert_to_rgba8: CV_32FC3 -> RGBA8", "[depth_utils]") {
    cv::Mat src(1, 1, CV_32FC3, cv::Scalar(1.0f, 0.0f, 0.5f));
    cv::Mat result = convert_to_rgba8(src);
    REQUIRE(result.depth() == CV_8U);
    REQUIRE(result.channels() == 4);
    REQUIRE(result.at<cv::Vec4b>(0, 0)[0] == 255);
    REQUIRE(result.at<cv::Vec4b>(0, 0)[1] == 0);
    REQUIRE(result.at<cv::Vec4b>(0, 0)[3] == 255);
}

TEST_CASE("convert_to_rgba8: unsupported channel count returns empty", "[depth_utils]") {
    cv::Mat src(2, 2, CV_8UC(2), cv::Scalar(10, 20));
    cv::Mat result = convert_to_rgba8(src);
    REQUIRE(result.empty());
}

// -- is_high_depth -----------------------------------------------------------

TEST_CASE("is_high_depth: CV_8U is not high depth", "[depth_utils]") {
    cv::Mat src(2, 2, CV_8UC3);
    REQUIRE_FALSE(is_high_depth(src));
}

TEST_CASE("is_high_depth: CV_16U is high depth", "[depth_utils]") {
    cv::Mat src(2, 2, CV_16UC3);
    REQUIRE(is_high_depth(src));
}

TEST_CASE("is_high_depth: CV_32F is high depth", "[depth_utils]") {
    cv::Mat src(2, 2, CV_32FC3);
    REQUIRE(is_high_depth(src));
}
