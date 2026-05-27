#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "core/depth_utils.h"

#include <opencv2/core.hpp>

using namespace idiff;

// -----------------------------------------------------------------------------
// convert_to_8u
// -----------------------------------------------------------------------------

TEST_CASE("convert_to_8u: empty input returns empty", "[depth_utils]") {
    cv::Mat empty;
    REQUIRE(convert_to_8u(empty).empty());
}

TEST_CASE("convert_to_8u: CV_8U inputs pass through unchanged",
          "[depth_utils]") {
    SECTION("multi-channel keeps the same buffer") {
        cv::Mat src(2, 2, CV_8UC3, cv::Scalar(100, 150, 200));
        cv::Mat r = convert_to_8u(src);
        REQUIRE(r.depth() == CV_8U);
        REQUIRE(r.channels() == 3);
        REQUIRE(r.data == src.data);
    }
    SECTION("single-channel preserves value") {
        cv::Mat src(2, 2, CV_8UC1, cv::Scalar(42));
        cv::Mat r = convert_to_8u(src);
        REQUIRE(r.depth() == CV_8U);
        REQUIRE(r.channels() == 1);
        REQUIRE(r.at<uint8_t>(0, 0) == 42);
    }
}

TEST_CASE("convert_to_8u: CV_16U scales 65535 -> 255 across channel counts",
          "[depth_utils]") {
    auto channels = GENERATE(1, 3, 4);
    CAPTURE(channels);
    cv::Mat src(1, 1, CV_MAKETYPE(CV_16U, channels));
    if (channels == 1) {
        src.at<uint16_t>(0, 0) = 65535;
    } else if (channels == 3) {
        src.at<cv::Vec3w>(0, 0) = cv::Vec3w(65535, 0, 32896);
    } else {
        src.at<cv::Vec4w>(0, 0) = cv::Vec4w(65535, 0, 32896, 65535);
    }

    cv::Mat r = convert_to_8u(src);
    REQUIRE(r.depth() == CV_8U);
    REQUIRE(r.channels() == channels);
    if (channels == 1) {
        REQUIRE(r.at<uint8_t>(0, 0) == 255);
    } else if (channels == 3) {
        REQUIRE(r.at<cv::Vec3b>(0, 0)[0] == 255);
        REQUIRE(r.at<cv::Vec3b>(0, 0)[1] == 0);
        REQUIRE(r.at<cv::Vec3b>(0, 0)[2] == 128);
    } else {
        REQUIRE(r.at<cv::Vec4b>(0, 0) == cv::Vec4b(255, 0, 128, 255));
    }
}

TEST_CASE("convert_to_8u: CV_16S shifts signed range to unsigned then scales",
          "[depth_utils]") {
    SECTION("CV_16SC3") {
        cv::Mat src(1, 1, CV_16SC3, cv::Scalar(-32768, 32767, 0));
        cv::Mat r = convert_to_8u(src);
        REQUIRE(r.depth() == CV_8U);
        REQUIRE(r.at<cv::Vec3b>(0, 0)[0] == 0);
        REQUIRE(r.at<cv::Vec3b>(0, 0)[1] == 255);
        REQUIRE(r.at<cv::Vec3b>(0, 0)[2] == 128);
    }
    SECTION("CV_16SC1") {
        cv::Mat src(2, 2, CV_16SC1, cv::Scalar(32767));
        cv::Mat r = convert_to_8u(src);
        REQUIRE(r.depth() == CV_8U);
        REQUIRE(r.channels() == 1);
        REQUIRE(r.at<uint8_t>(0, 0) == 255);
    }
}

TEST_CASE("convert_to_8u: CV_32F maps and clamps [0,1] to [0,255]",
          "[depth_utils]") {
    SECTION("CV_32FC3 normal range") {
        cv::Mat src(1, 1, CV_32FC3, cv::Scalar(1.0f, 0.0f, 0.5f));
        cv::Mat r = convert_to_8u(src);
        REQUIRE(r.depth() == CV_8U);
        REQUIRE(r.at<cv::Vec3b>(0, 0)[0] == 255);
        REQUIRE(r.at<cv::Vec3b>(0, 0)[1] == 0);
        // 0.5 * 255 rounds to 127 or 128.
        REQUIRE(r.at<cv::Vec3b>(0, 0)[2] >= 127);
        REQUIRE(r.at<cv::Vec3b>(0, 0)[2] <= 128);
    }
    SECTION("CV_32FC1 keeps shape") {
        cv::Mat src(2, 2, CV_32FC1, cv::Scalar(0.75f));
        cv::Mat r = convert_to_8u(src);
        REQUIRE(r.depth() == CV_8U);
        REQUIRE(r.channels() == 1);
        REQUIRE(r.at<uint8_t>(0, 0) >= 191);
        REQUIRE(r.at<uint8_t>(0, 0) <= 192);
    }
    SECTION("CV_32FC3 clamps below 0 and above 1") {
        cv::Mat src(1, 1, CV_32FC3, cv::Scalar(2.5f, -0.5f, 0.5f));
        cv::Mat r = convert_to_8u(src);
        REQUIRE(r.at<cv::Vec3b>(0, 0)[0] == 255);
        REQUIRE(r.at<cv::Vec3b>(0, 0)[1] == 0);
    }
}

// -----------------------------------------------------------------------------
// convert_to_rgba8
// -----------------------------------------------------------------------------

TEST_CASE("convert_to_rgba8: empty and unsupported inputs return empty",
          "[depth_utils]") {
    cv::Mat empty;
    REQUIRE(convert_to_rgba8(empty).empty());

    cv::Mat two_channel(2, 2, CV_8UC(2), cv::Scalar(10, 20));
    REQUIRE(convert_to_rgba8(two_channel).empty());
}

TEST_CASE("convert_to_rgba8: produces RGBA8 output across input shapes",
          "[depth_utils]") {
    SECTION("CV_8UC1 broadcasts to RGB and sets alpha 255") {
        cv::Mat src(2, 2, CV_8UC1, cv::Scalar(128));
        cv::Mat r = convert_to_rgba8(src);
        REQUIRE(r.depth() == CV_8U);
        REQUIRE(r.channels() == 4);
        REQUIRE(r.at<cv::Vec4b>(0, 0) == cv::Vec4b(128, 128, 128, 255));
    }
    SECTION("CV_8UC3 adds alpha 255") {
        cv::Mat src(2, 2, CV_8UC3, cv::Scalar(10, 20, 30));
        cv::Mat r = convert_to_rgba8(src);
        REQUIRE(r.at<cv::Vec4b>(0, 0) == cv::Vec4b(10, 20, 30, 255));
    }
    SECTION("CV_8UC4 passes through") {
        cv::Mat src(2, 2, CV_8UC4, cv::Scalar(10, 20, 30, 128));
        cv::Mat r = convert_to_rgba8(src);
        REQUIRE(r.at<cv::Vec4b>(0, 0) == cv::Vec4b(10, 20, 30, 128));
    }
    SECTION("CV_16UC3 scales then adds alpha 255") {
        cv::Mat src(1, 1, CV_16UC3, cv::Scalar(65535, 0, 32896));
        cv::Mat r = convert_to_rgba8(src);
        REQUIRE(r.at<cv::Vec4b>(0, 0) == cv::Vec4b(255, 0, 128, 255));
    }
    SECTION("CV_16UC1 broadcasts and sets alpha 255") {
        cv::Mat src(1, 1, CV_16UC1, cv::Scalar(65535));
        cv::Mat r = convert_to_rgba8(src);
        REQUIRE(r.at<cv::Vec4b>(0, 0)[0] == 255);
        REQUIRE(r.at<cv::Vec4b>(0, 0)[3] == 255);
    }
    SECTION("CV_16SC3 shifts signed to unsigned and adds alpha 255") {
        cv::Mat src(1, 1, CV_16SC3, cv::Scalar(32767, -32768, 0));
        cv::Mat r = convert_to_rgba8(src);
        REQUIRE(r.at<cv::Vec4b>(0, 0)[0] == 255);
        REQUIRE(r.at<cv::Vec4b>(0, 0)[1] == 0);
        REQUIRE(r.at<cv::Vec4b>(0, 0)[3] == 255);
    }
    SECTION("CV_32FC3 maps [0,1] then adds alpha 255") {
        cv::Mat src(1, 1, CV_32FC3, cv::Scalar(1.0f, 0.0f, 0.5f));
        cv::Mat r = convert_to_rgba8(src);
        REQUIRE(r.at<cv::Vec4b>(0, 0)[0] == 255);
        REQUIRE(r.at<cv::Vec4b>(0, 0)[1] == 0);
        REQUIRE(r.at<cv::Vec4b>(0, 0)[3] == 255);
    }
}

// -----------------------------------------------------------------------------
// is_high_depth
// -----------------------------------------------------------------------------

TEST_CASE("is_high_depth: only > CV_8U is considered high depth",
          "[depth_utils]") {
    REQUIRE_FALSE(is_high_depth(cv::Mat(2, 2, CV_8UC3)));
    REQUIRE(is_high_depth(cv::Mat(2, 2, CV_16UC3)));
    REQUIRE(is_high_depth(cv::Mat(2, 2, CV_16SC3)));
    REQUIRE(is_high_depth(cv::Mat(2, 2, CV_32FC3)));
}
