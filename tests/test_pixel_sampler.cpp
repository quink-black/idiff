#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "app/pixel_sampler.h"

#include <opencv2/core.hpp>

#include <cstring>
#include <string>

using namespace idiff;

// -----------------------------------------------------------------------------
// sample_image: input validation.
// -----------------------------------------------------------------------------

TEST_CASE("sample_image: rejects invalid inputs", "[pixel_sampler]") {
    SECTION("empty matrix") {
        cv::Mat empty;
        REQUIRE_FALSE(sample_image(empty, 0.5, 0.5).valid);
    }

    cv::Mat m(4, 4, CV_8UC3, cv::Scalar(10, 20, 30));
    SECTION("u or v at or above 1.0") {
        REQUIRE_FALSE(sample_image(m, 1.0, 0.5).valid);
        REQUIRE_FALSE(sample_image(m, 0.5, 1.0).valid);
    }
    SECTION("negative u or v") {
        REQUIRE_FALSE(sample_image(m, -0.1, 0.5).valid);
        REQUIRE_FALSE(sample_image(m, 0.5, -0.1).valid);
    }
}

// -----------------------------------------------------------------------------
// sample_image: depth and channel-count coverage.
// -----------------------------------------------------------------------------

TEST_CASE("sample_image: extracts the right pixel across depths and channels",
          "[pixel_sampler]") {
    SECTION("CV_8UC1 picks the correct pixel") {
        cv::Mat m(2, 2, CV_8UC1);
        m.at<uint8_t>(0, 0) = 10;
        m.at<uint8_t>(0, 1) = 20;
        m.at<uint8_t>(1, 0) = 30;
        m.at<uint8_t>(1, 1) = 40;

        auto s = sample_image(m, 0.25, 0.25);
        REQUIRE(s.valid);
        REQUIRE(s.channels == 1);
        REQUIRE(s.depth == CV_8U);
        REQUIRE(s.v[0] == 10);

        s = sample_image(m, 0.75, 0.75);
        REQUIRE(s.v[0] == 40);
    }

    SECTION("CV_8UC3 picks the correct pixel and reports 3 channels") {
        cv::Mat m(1, 2, CV_8UC3);
        m.at<cv::Vec3b>(0, 0) = cv::Vec3b(10, 20, 30);
        m.at<cv::Vec3b>(0, 1) = cv::Vec3b(40, 50, 60);

        auto s = sample_image(m, 0.25, 0.5);
        REQUIRE(s.valid);
        REQUIRE(s.channels == 3);
        REQUIRE(s.v[0] == 10);
        REQUIRE(s.v[1] == 20);
        REQUIRE(s.v[2] == 30);

        s = sample_image(m, 0.75, 0.5);
        REQUIRE(s.v[0] == 40);
        REQUIRE(s.v[1] == 50);
        REQUIRE(s.v[2] == 60);
    }

    SECTION("CV_8UC4 carries the alpha channel") {
        cv::Mat m(1, 1, CV_8UC4);
        m.at<cv::Vec4b>(0, 0) = cv::Vec4b(1, 2, 3, 4);
        auto s = sample_image(m, 0.5, 0.5);
        REQUIRE(s.valid);
        REQUIRE(s.channels == 4);
        REQUIRE(s.v[0] == 1);
        REQUIRE(s.v[1] == 2);
        REQUIRE(s.v[2] == 3);
        REQUIRE(s.v[3] == 4);
    }

    SECTION("CV_16UC3 carries the full 16-bit value range") {
        cv::Mat m(1, 1, CV_16UC3);
        m.at<cv::Vec3w>(0, 0) = cv::Vec3w(0, 32768, 65535);
        auto s = sample_image(m, 0.5, 0.5);
        REQUIRE(s.valid);
        REQUIRE(s.depth == CV_16U);
        REQUIRE(s.v[0] == 0);
        REQUIRE(s.v[1] == 32768);
        REQUIRE(s.v[2] == 65535);
    }

    SECTION("CV_32FC3 preserves float values") {
        cv::Mat m(1, 1, CV_32FC3);
        m.at<cv::Vec3f>(0, 0) = cv::Vec3f(0.25f, 0.5f, 1.5f);
        auto s = sample_image(m, 0.5, 0.5);
        REQUIRE(s.valid);
        REQUIRE(s.depth == CV_32F);
        REQUIRE(s.channels == 3);
        REQUIRE(s.v[0] == Catch::Approx(0.25));
        REQUIRE(s.v[1] == Catch::Approx(0.5));
        REQUIRE(s.v[2] == Catch::Approx(1.5));
    }
}

// -----------------------------------------------------------------------------
// pixel_to_norm / norm_to_pixel.
// -----------------------------------------------------------------------------

TEST_CASE("pixel <-> norm coordinate conversion", "[pixel_sampler]") {
    SECTION("pixel_to_norm uses the pixel-center convention") {
        REQUIRE(pixel_to_norm(0, 4) == Catch::Approx(0.125));
        REQUIRE(pixel_to_norm(3, 4) == Catch::Approx(0.875));
    }
    SECTION("pixel_to_norm with w == 0 returns 0.5 instead of dividing") {
        REQUIRE(pixel_to_norm(0, 0) == Catch::Approx(0.5));
    }
    SECTION("norm_to_pixel maps to the expected pixel index") {
        REQUIRE(norm_to_pixel(0.0, 4) == 0);
        REQUIRE(norm_to_pixel(0.5, 4) == 2);
        REQUIRE(norm_to_pixel(0.999, 4) == 3);
    }
    SECTION("norm_to_pixel clamps out-of-range input to a valid index") {
        REQUIRE(norm_to_pixel(-1.0, 4) == 0);
        // 1.0 must map to the last valid index, not w.
        REQUIRE(norm_to_pixel(1.0, 4) == 3);
    }
    SECTION("norm_to_pixel . pixel_to_norm is identity for every index") {
        for (int w : {1, 3, 8, 256, 1024}) {
            for (int x = 0; x < w; ++x) {
                double u = pixel_to_norm(x, w);
                REQUIRE(norm_to_pixel(u, w) == x);
            }
        }
    }
}

// -----------------------------------------------------------------------------
// format_pixel.
// -----------------------------------------------------------------------------

TEST_CASE("format_pixel: rendering across depths and channel counts",
          "[pixel_sampler]") {
    char buf[64] = {};

    SECTION("invalid sample renders an em-dash") {
        PixelSample s;
        REQUIRE(format_pixel(s, buf, sizeof(buf)));
        REQUIRE(std::string(buf) == "\xE2\x80\x94");
    }
    SECTION("8U single channel renders as int") {
        PixelSample s;
        s.valid = true;
        s.channels = 1;
        s.depth = CV_8U;
        s.v[0] = 200;
        REQUIRE(format_pixel(s, buf, sizeof(buf)));
        REQUIRE(std::string(buf) == "200");
    }
    SECTION("8U RGB renders as a tuple") {
        PixelSample s;
        s.valid = true;
        s.channels = 3;
        s.depth = CV_8U;
        s.v[0] = 10;
        s.v[1] = 20;
        s.v[2] = 30;
        REQUIRE(format_pixel(s, buf, sizeof(buf)));
        REQUIRE(std::string(buf) == "(10, 20, 30)");
    }
    SECTION("32F prints 4 decimals") {
        PixelSample s;
        s.valid = true;
        s.channels = 1;
        s.depth = CV_32F;
        s.v[0] = 0.123456;
        REQUIRE(format_pixel(s, buf, sizeof(buf)));
        REQUIRE(std::string(buf) == "0.1235");
    }
    SECTION("unsupported depth is tagged and reports false") {
        PixelSample s;
        s.valid = true;
        s.channels = 3;
        s.depth = CV_64F;
        REQUIRE_FALSE(format_pixel(s, buf, sizeof(buf)));
        REQUIRE(std::string(buf) == "(unsupported depth)");
    }
}

// -----------------------------------------------------------------------------
// format_delta.
// -----------------------------------------------------------------------------

TEST_CASE("format_delta: signed deltas, mismatches and float formatting",
          "[pixel_sampler]") {
    char buf[64] = {};

    SECTION("identical samples produce zeros, not em-dash") {
        PixelSample a;
        a.valid = true;
        a.channels = 3;
        a.depth = CV_8U;
        a.v[0] = 100;
        a.v[1] = 100;
        a.v[2] = 100;
        REQUIRE(format_delta(a, a, buf, sizeof(buf)));
        REQUIRE(std::string(buf) == "(0, 0, 0)");
    }
    SECTION("signed integer delta has explicit sign per channel") {
        PixelSample a;
        a.valid = true;
        a.channels = 3;
        a.depth = CV_8U;
        a.v[0] = 100;
        a.v[1] = 50;
        a.v[2] = 200;
        PixelSample b;
        b.valid = true;
        b.channels = 3;
        b.depth = CV_8U;
        b.v[0] = 90;
        b.v[1] = 60;
        b.v[2] = 200;
        REQUIRE(format_delta(a, b, buf, sizeof(buf)));
        REQUIRE(std::string(buf) == "(+10, -10, 0)");
    }
    SECTION("32F prints 4 decimals with explicit sign") {
        PixelSample a;
        a.valid = true;
        a.channels = 1;
        a.depth = CV_32F;
        a.v[0] = 0.6;
        PixelSample b;
        b.valid = true;
        b.channels = 1;
        b.depth = CV_32F;
        b.v[0] = 0.5;
        REQUIRE(format_delta(a, b, buf, sizeof(buf)));
        REQUIRE(std::string(buf) == "+0.1000");
    }
    SECTION("mismatched samples render an em-dash") {
        // depth mismatch
        PixelSample a;
        a.valid = true;
        a.channels = 3;
        a.depth = CV_8U;
        PixelSample b;
        b.valid = true;
        b.channels = 3;
        b.depth = CV_16U;
        REQUIRE_FALSE(format_delta(a, b, buf, sizeof(buf)));
        REQUIRE(std::string(buf) == "\xE2\x80\x94");

        // channel mismatch
        b.depth = CV_8U;
        b.channels = 4;
        REQUIRE_FALSE(format_delta(a, b, buf, sizeof(buf)));
        REQUIRE(std::string(buf) == "\xE2\x80\x94");

        // invalid reference sample
        PixelSample invalid;  // valid=false
        REQUIRE_FALSE(format_delta(a, invalid, buf, sizeof(buf)));
        REQUIRE(std::string(buf) == "\xE2\x80\x94");
    }
}

TEST_CASE("norm_to_pixel: out-of-range input is clamped, not undefined",
          "[pixel_sampler]") {
    REQUIRE(norm_to_pixel(-1.0, 4) == 0);
    // 1.0 should map to the last valid index, not w.
    REQUIRE(norm_to_pixel(1.0, 4) == 3);
}

// -- format_pixel --------------------------------------------------------

TEST_CASE("format_pixel: invalid renders em-dash", "[pixel_sampler]") {
    PixelSample s;
    char buf[32] = {};
    REQUIRE(format_pixel(s, buf, sizeof(buf)));
    REQUIRE(std::string(buf) == "\xE2\x80\x94");
}

TEST_CASE("format_pixel: 8U single channel renders as int", "[pixel_sampler]") {
    PixelSample s;
    s.valid = true; s.channels = 1; s.depth = CV_8U; s.v[0] = 200;
    char buf[32] = {};
    REQUIRE(format_pixel(s, buf, sizeof(buf)));
    REQUIRE(std::string(buf) == "200");
}

TEST_CASE("format_pixel: 8U RGB renders as tuple", "[pixel_sampler]") {
    PixelSample s;
    s.valid = true; s.channels = 3; s.depth = CV_8U;
    s.v[0] = 10; s.v[1] = 20; s.v[2] = 30;
    char buf[32] = {};
    REQUIRE(format_pixel(s, buf, sizeof(buf)));
    REQUIRE(std::string(buf) == "(10, 20, 30)");
}

TEST_CASE("format_pixel: 32F uses 4 decimals", "[pixel_sampler]") {
    PixelSample s;
    s.valid = true; s.channels = 1; s.depth = CV_32F; s.v[0] = 0.123456;
    char buf[32] = {};
    REQUIRE(format_pixel(s, buf, sizeof(buf)));
    REQUIRE(std::string(buf) == "0.1235");
}

TEST_CASE("format_pixel: unsupported depth tagged", "[pixel_sampler]") {
    PixelSample s;
    s.valid = true; s.channels = 3; s.depth = CV_64F;
    char buf[64] = {};
    REQUIRE_FALSE(format_pixel(s, buf, sizeof(buf)));
    REQUIRE(std::string(buf) == "(unsupported depth)");
}

// -- format_delta --------------------------------------------------------

TEST_CASE("format_delta: identical samples produce zeros, not em-dash",
          "[pixel_sampler]") {
    PixelSample a;
    a.valid = true; a.channels = 3; a.depth = CV_8U;
    a.v[0] = 100; a.v[1] = 100; a.v[2] = 100;

    char buf[32] = {};
    REQUIRE(format_delta(a, a, buf, sizeof(buf)));
    REQUIRE(std::string(buf) == "(0, 0, 0)");
}

TEST_CASE("format_delta: signed integer delta has explicit sign",
          "[pixel_sampler]") {
    PixelSample a; a.valid = true; a.channels = 3; a.depth = CV_8U;
    a.v[0] = 100; a.v[1] = 50; a.v[2] = 200;
    PixelSample b; b.valid = true; b.channels = 3; b.depth = CV_8U;
    b.v[0] = 90;  b.v[1] = 60; b.v[2] = 200;

    char buf[64] = {};
    REQUIRE(format_delta(a, b, buf, sizeof(buf)));
    REQUIRE(std::string(buf) == "(+10, -10, 0)");
}

TEST_CASE("format_delta: depth mismatch -> em-dash", "[pixel_sampler]") {
    PixelSample a; a.valid = true; a.channels = 3; a.depth = CV_8U;
    PixelSample b; b.valid = true; b.channels = 3; b.depth = CV_16U;
    char buf[32] = {};
    REQUIRE_FALSE(format_delta(a, b, buf, sizeof(buf)));
    REQUIRE(std::string(buf) == "\xE2\x80\x94");
}

TEST_CASE("format_delta: channel mismatch -> em-dash", "[pixel_sampler]") {
    PixelSample a; a.valid = true; a.channels = 3; a.depth = CV_8U;
    PixelSample b; b.valid = true; b.channels = 4; b.depth = CV_8U;
    char buf[32] = {};
    REQUIRE_FALSE(format_delta(a, b, buf, sizeof(buf)));
    REQUIRE(std::string(buf) == "\xE2\x80\x94");
}

TEST_CASE("format_delta: invalid ref -> em-dash", "[pixel_sampler]") {
    PixelSample a; a.valid = true; a.channels = 3; a.depth = CV_8U;
    PixelSample b; // invalid
    char buf[32] = {};
    REQUIRE_FALSE(format_delta(a, b, buf, sizeof(buf)));
    REQUIRE(std::string(buf) == "\xE2\x80\x94");
}

TEST_CASE("format_delta: 32F prints 4 decimals with sign", "[pixel_sampler]") {
    PixelSample a; a.valid = true; a.channels = 1; a.depth = CV_32F; a.v[0] = 0.6;
    PixelSample b; b.valid = true; b.channels = 1; b.depth = CV_32F; b.v[0] = 0.5;
    char buf[32] = {};
    REQUIRE(format_delta(a, b, buf, sizeof(buf)));
    REQUIRE(std::string(buf) == "+0.1000");
}
