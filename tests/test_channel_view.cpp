#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "core/channel_view.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <string>
#include <vector>

using namespace idiff;

namespace {

cv::Mat make_rgb8(int rows, int cols, cv::Vec3b v) {
    return cv::Mat(rows, cols, CV_8UC3, cv::Scalar(v[0], v[1], v[2]));
}

cv::Mat make_rgba8(int rows, int cols, cv::Vec4b v) {
    return cv::Mat(rows, cols, CV_8UC4, cv::Scalar(v[0], v[1], v[2], v[3]));
}

}  // namespace

// -----------------------------------------------------------------------------
// Labels and predicates: cheap pure functions, batched.
// -----------------------------------------------------------------------------

TEST_CASE("ChannelView: mode and background labels", "[channel_view]") {
    REQUIRE(std::string(channel_view_mode_label(ChannelViewMode::None)) == "All");
    REQUIRE(std::string(channel_view_mode_label(ChannelViewMode::AlphaGray)) ==
            "Alpha (Gray)");
    REQUIRE(std::string(channel_view_mode_label(ChannelViewMode::RGB)) == "RGB");
    REQUIRE(std::string(channel_view_mode_label(ChannelViewMode::AlphaContour)) ==
            "Alpha Contour");

    REQUIRE(std::string(view_background_label(ViewBackground::Black)) == "Black");
    REQUIRE(std::string(view_background_label(ViewBackground::White)) == "White");
    REQUIRE(std::string(view_background_label(ViewBackground::Red)) == "Red");
    REQUIRE(std::string(view_background_label(ViewBackground::Green)) == "Green");
    REQUIRE(std::string(view_background_label(ViewBackground::Blue)) == "Blue");
    REQUIRE(std::string(view_background_label(ViewBackground::DarkChecker)) ==
            "Dark Checker");
    REQUIRE(std::string(view_background_label(ViewBackground::LightChecker)) ==
            "Light Checker");
}

TEST_CASE("ChannelView: requires_alpha identifies alpha-only modes",
          "[channel_view]") {
    REQUIRE(channel_view_requires_alpha(ChannelViewMode::AlphaGray));
    REQUIRE(channel_view_requires_alpha(ChannelViewMode::AlphaContour));
    REQUIRE_FALSE(channel_view_requires_alpha(ChannelViewMode::None));
    REQUIRE_FALSE(channel_view_requires_alpha(ChannelViewMode::RGB));
    REQUIRE_FALSE(channel_view_requires_alpha(ChannelViewMode::R));
}

// -----------------------------------------------------------------------------
// Empty input / failure paths.
// -----------------------------------------------------------------------------

TEST_CASE("ChannelView: empty image returns nullopt", "[channel_view]") {
    cv::Mat empty;
    REQUIRE_FALSE(extract_channel_view(empty, ChannelViewMode::R,
                                       ViewBackground::Black)
                      .has_value());
}

TEST_CASE("ChannelView: rejects modes incompatible with input shape",
          "[channel_view]") {
    cv::Mat gray(2, 2, CV_8UC1, cv::Scalar(128));
    cv::Mat rgb = make_rgb8(2, 2, cv::Vec3b(10, 20, 30));

    auto mode = GENERATE(ChannelViewMode::R, ChannelViewMode::G,
                         ChannelViewMode::B, ChannelViewMode::Y,
                         ChannelViewMode::U, ChannelViewMode::V);
    CAPTURE(static_cast<int>(mode));
    REQUIRE_FALSE(
        extract_channel_view(gray, mode, ViewBackground::Black).has_value());

    auto alpha_only = GENERATE(ChannelViewMode::AlphaGray,
                               ChannelViewMode::AlphaContour);
    CAPTURE(static_cast<int>(alpha_only));
    REQUIRE_FALSE(extract_channel_view(rgb, alpha_only, ViewBackground::Black)
                      .has_value());
}

// -----------------------------------------------------------------------------
// None-mode compositing: 8-bit and 16-bit, all backgrounds.
// -----------------------------------------------------------------------------

TEST_CASE("ChannelView: None returns clone of non-alpha RGB at native depth",
          "[channel_view]") {
    SECTION("CV_8UC3") {
        cv::Mat rgb = make_rgb8(4, 4, cv::Vec3b(10, 20, 30));
        auto r = extract_channel_view(rgb, ChannelViewMode::None,
                                      ViewBackground::Black);
        REQUIRE(r.has_value());
        REQUIRE(r->channels() == 3);
        REQUIRE(r->depth() == CV_8U);
        REQUIRE(r->at<cv::Vec3b>(0, 0) == cv::Vec3b(10, 20, 30));
    }
    SECTION("CV_16UC3 keeps depth") {
        cv::Mat rgb16(4, 4, CV_16UC3, cv::Scalar(10000, 20000, 30000));
        auto r = extract_channel_view(rgb16, ChannelViewMode::None,
                                      ViewBackground::Black);
        REQUIRE(r.has_value());
        REQUIRE(r->depth() == CV_16U);
        REQUIRE(r->channels() == 3);
    }
}

TEST_CASE("ChannelView: None composites RGBA on every background",
          "[channel_view]") {
    auto bg = GENERATE(ViewBackground::Black, ViewBackground::White,
                       ViewBackground::DarkChecker, ViewBackground::LightChecker);
    CAPTURE(static_cast<int>(bg));

    auto run = [&](const cv::Mat& rgba) {
        auto result =
            extract_channel_view(rgba, ChannelViewMode::None, bg);
        REQUIRE(result.has_value());
        REQUIRE(result->channels() == 4);
        // Output is always 8-bit RGBA after compositing.
        REQUIRE(result->depth() == CV_8U);
        // Opaque pixel keeps original color.
        cv::Vec4b p00 = result->at<cv::Vec4b>(0, 0);
        REQUIRE(p00[3] == 255);
        // Transparent pixel takes background contribution.
        cv::Vec4b p01 = result->at<cv::Vec4b>(0, 1);
        REQUIRE(p01[3] == 255);
        if (bg == ViewBackground::Black) {
            REQUIRE(p01 == cv::Vec4b(0, 0, 0, 255));
        } else if (bg == ViewBackground::White) {
            REQUIRE(p01[0] == 255);
            REQUIRE(p01[1] == 255);
            REQUIRE(p01[2] == 255);
        } else {
            // Checker: at least one channel non-zero.
            REQUIRE(p01[0] > 0);
        }
    };

    SECTION("CV_8UC4") {
        cv::Mat rgba(16, 16, CV_8UC4);
        rgba.at<cv::Vec4b>(0, 0) = cv::Vec4b(200, 100, 50, 255);
        rgba.at<cv::Vec4b>(0, 1) = cv::Vec4b(0, 0, 0, 0);
        run(rgba);
        // Sanity: opaque pixel keeps exact RGB.
        auto result = extract_channel_view(rgba, ChannelViewMode::None, bg);
        REQUIRE(result->at<cv::Vec4b>(0, 0)[0] == 200);
    }
    SECTION("CV_16UC4 downsamples to 8-bit") {
        cv::Mat rgba16(16, 16, CV_16UC4, cv::Scalar(0, 0, 0, 0));
        rgba16.at<cv::Vec4w>(0, 0) = cv::Vec4w(65535, 32768, 0, 65535);
        run(rgba16);
        auto result = extract_channel_view(rgba16, ChannelViewMode::None, bg);
        REQUIRE(result->at<cv::Vec4b>(0, 0)[0] == 255);
    }
}

// -----------------------------------------------------------------------------
// R/G/B extraction: 8-bit and 16-bit, all three channels.
// -----------------------------------------------------------------------------

TEST_CASE("ChannelView: R/G/B extraction at every supported depth",
          "[channel_view]") {
    struct Case {
        ChannelViewMode mode;
        int channel_index;
    };
    auto c = GENERATE(Case{ChannelViewMode::R, 0}, Case{ChannelViewMode::G, 1},
                      Case{ChannelViewMode::B, 2});
    CAPTURE(static_cast<int>(c.mode));

    SECTION("CV_8UC3") {
        cv::Mat rgb(2, 2, CV_8UC3);
        rgb.at<cv::Vec3b>(0, 0) = cv::Vec3b(10, 21, 32);
        auto r = extract_channel_view(rgb, c.mode, ViewBackground::Black);
        REQUIRE(r.has_value());
        REQUIRE(r->channels() == 1);
        REQUIRE(r->depth() == CV_8U);
        REQUIRE(r->at<uint8_t>(0, 0) == cv::Vec3b(10, 21, 32)[c.channel_index]);
    }
    SECTION("CV_16UC3 preserves depth") {
        cv::Mat rgb16(2, 2, CV_16UC3);
        rgb16.at<cv::Vec3w>(0, 0) = cv::Vec3w(1000, 2000, 3000);
        auto r = extract_channel_view(rgb16, c.mode, ViewBackground::Black);
        REQUIRE(r.has_value());
        REQUIRE(r->depth() == CV_16U);
        REQUIRE(r->at<uint16_t>(0, 0) ==
                cv::Vec3w(1000, 2000, 3000)[c.channel_index]);
    }
    SECTION("CV_8UC4 source drops alpha") {
        cv::Mat rgba(2, 2, CV_8UC4);
        rgba.at<cv::Vec4b>(0, 0) = cv::Vec4b(10, 21, 32, 128);
        auto r = extract_channel_view(rgba, c.mode, ViewBackground::Black);
        REQUIRE(r.has_value());
        REQUIRE(r->at<uint8_t>(0, 0) == cv::Vec4b(10, 21, 32, 128)[c.channel_index]);
    }
}

// -----------------------------------------------------------------------------
// RGB mode: drops alpha, keeps depth, returns clone for plain RGB.
// -----------------------------------------------------------------------------

TEST_CASE("ChannelView: RGB drops alpha and keeps depth", "[channel_view]") {
    SECTION("CV_8UC3 returns clone") {
        cv::Mat rgb = make_rgb8(2, 2, cv::Vec3b(10, 20, 30));
        auto r = extract_channel_view(rgb, ChannelViewMode::RGB,
                                      ViewBackground::Black);
        REQUIRE(r.has_value());
        REQUIRE(r->channels() == 3);
        REQUIRE(r->at<cv::Vec3b>(0, 0) == cv::Vec3b(10, 20, 30));
    }
    SECTION("CV_8UC4 drops alpha") {
        cv::Mat rgba(2, 2, CV_8UC4);
        rgba.at<cv::Vec4b>(0, 0) = cv::Vec4b(10, 20, 30, 128);
        rgba.at<cv::Vec4b>(0, 1) = cv::Vec4b(40, 50, 60, 0);
        auto r = extract_channel_view(rgba, ChannelViewMode::RGB,
                                      ViewBackground::Black);
        REQUIRE(r.has_value());
        REQUIRE(r->channels() == 3);
        REQUIRE(r->at<cv::Vec3b>(0, 0) == cv::Vec3b(10, 20, 30));
        REQUIRE(r->at<cv::Vec3b>(0, 1) == cv::Vec3b(40, 50, 60));
    }
    SECTION("CV_16UC4 drops alpha and keeps 16-bit depth") {
        cv::Mat rgba16(2, 2, CV_16UC4);
        rgba16.at<cv::Vec4w>(0, 0) = cv::Vec4w(1000, 2000, 3000, 4000);
        auto r = extract_channel_view(rgba16, ChannelViewMode::RGB,
                                      ViewBackground::Black);
        REQUIRE(r.has_value());
        REQUIRE(r->channels() == 3);
        REQUIRE(r->depth() == CV_16U);
        REQUIRE(r->at<cv::Vec3w>(0, 0) == cv::Vec3w(1000, 2000, 3000));
    }
}

// -----------------------------------------------------------------------------
// Alpha views.
// -----------------------------------------------------------------------------

TEST_CASE("ChannelView: AlphaGray exposes alpha channel at source depth",
          "[channel_view]") {
    SECTION("CV_8UC4") {
        cv::Mat rgba(2, 2, CV_8UC4);
        rgba.at<cv::Vec4b>(0, 0) = cv::Vec4b(10, 20, 30, 128);
        rgba.at<cv::Vec4b>(0, 1) = cv::Vec4b(40, 50, 60, 255);
        rgba.at<cv::Vec4b>(1, 0) = cv::Vec4b(70, 80, 90, 0);
        rgba.at<cv::Vec4b>(1, 1) = cv::Vec4b(100, 110, 120, 64);
        auto r = extract_channel_view(rgba, ChannelViewMode::AlphaGray,
                                      ViewBackground::Black);
        REQUIRE(r.has_value());
        REQUIRE(r->channels() == 1);
        REQUIRE(r->depth() == CV_8U);
        REQUIRE(r->at<uint8_t>(0, 0) == 128);
        REQUIRE(r->at<uint8_t>(0, 1) == 255);
        REQUIRE(r->at<uint8_t>(1, 0) == 0);
        REQUIRE(r->at<uint8_t>(1, 1) == 64);
    }
    SECTION("CV_16UC4 preserves depth") {
        cv::Mat rgba16(2, 2, CV_16UC4);
        rgba16.at<cv::Vec4w>(0, 0) = cv::Vec4w(1000, 2000, 3000, 4000);
        auto r = extract_channel_view(rgba16, ChannelViewMode::AlphaGray,
                                      ViewBackground::Black);
        REQUIRE(r.has_value());
        REQUIRE(r->depth() == CV_16U);
        REQUIRE(r->at<uint16_t>(0, 0) == 4000);
    }
}

TEST_CASE("ChannelView: AlphaContour produces RGBA8 with red contour",
          "[channel_view]") {
    auto check_contour = [](cv::Mat rgba) {
        auto result = extract_channel_view(rgba, ChannelViewMode::AlphaContour,
                                           ViewBackground::Black);
        REQUIRE(result.has_value());
        REQUIRE(result->channels() == 4);
        REQUIRE(result->depth() == CV_8U);

        bool found_red = false;
        for (int y = 0; y < rgba.rows; ++y) {
            cv::Vec4b p = result->at<cv::Vec4b>(y, rgba.cols / 2 - 1);
            if (p[0] == 255 && p[1] == 0 && p[2] == 0 && p[3] == 255) {
                found_red = true;
                break;
            }
        }
        REQUIRE(found_red);
    };

    SECTION("CV_8UC4") {
        cv::Mat rgba(8, 8, CV_8UC4, cv::Vec4b(100, 150, 200, 255));
        for (int y = 0; y < 8; ++y)
            for (int x = 4; x < 8; ++x)
                rgba.at<cv::Vec4b>(y, x) = cv::Vec4b(100, 150, 200, 0);
        check_contour(rgba);
    }
    SECTION("CV_16UC4 downsamples to 8-bit") {
        cv::Mat rgba16(8, 8, CV_16UC4,
                       cv::Scalar(40000, 50000, 60000, 65535));
        for (int y = 0; y < 8; ++y)
            for (int x = 4; x < 8; ++x)
                rgba16.at<cv::Vec4w>(y, x) = cv::Vec4w(40000, 50000, 60000, 0);
        check_contour(rgba16);
    }
}

// -----------------------------------------------------------------------------
// YUV views.
// -----------------------------------------------------------------------------

TEST_CASE("ChannelView: Y/U/V match cv::cvtColor output", "[channel_view]") {
    cv::Mat rgb(2, 2, CV_8UC3);
    rgb.at<cv::Vec3b>(0, 0) = cv::Vec3b(255, 0, 0);
    rgb.at<cv::Vec3b>(0, 1) = cv::Vec3b(0, 255, 0);
    rgb.at<cv::Vec3b>(1, 0) = cv::Vec3b(0, 0, 255);
    rgb.at<cv::Vec3b>(1, 1) = cv::Vec3b(255, 255, 255);

    cv::Mat yuv;
    cv::cvtColor(rgb, yuv, cv::COLOR_RGB2YUV);
    std::vector<cv::Mat> planes;
    cv::split(yuv, planes);

    struct Case {
        ChannelViewMode mode;
        int plane;
    };
    auto c = GENERATE(Case{ChannelViewMode::Y, 0}, Case{ChannelViewMode::U, 1},
                      Case{ChannelViewMode::V, 2});
    CAPTURE(static_cast<int>(c.mode));

    auto r = extract_channel_view(rgb, c.mode, ViewBackground::Black);
    REQUIRE(r.has_value());
    REQUIRE(r->channels() == 1);
    REQUIRE(r->depth() == CV_8U);
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x)
            REQUIRE(r->at<uint8_t>(y, x) == planes[c.plane].at<uint8_t>(y, x));
}

TEST_CASE("ChannelView: Y is 8-bit even for 16-bit RGB and RGBA sources",
          "[channel_view]") {
    SECTION("CV_16UC3") {
        cv::Mat rgb16(4, 4, CV_16UC3, cv::Scalar(65535, 32768, 0));
        auto r = extract_channel_view(rgb16, ChannelViewMode::Y,
                                      ViewBackground::Black);
        REQUIRE(r.has_value());
        REQUIRE(r->depth() == CV_8U);
        REQUIRE(r->channels() == 1);
    }
    SECTION("CV_8UC4 drops alpha before YUV conversion") {
        cv::Mat rgba(2, 2, CV_8UC4);
        rgba.at<cv::Vec4b>(0, 0) = cv::Vec4b(255, 128, 64, 128);
        auto r = extract_channel_view(rgba, ChannelViewMode::Y,
                                      ViewBackground::Black);
        REQUIRE(r.has_value());

        cv::Mat rgb;
        cv::cvtColor(rgba, rgb, cv::COLOR_RGBA2RGB);
        cv::Mat yuv;
        cv::cvtColor(rgb, yuv, cv::COLOR_RGB2YUV);
        std::vector<cv::Mat> planes;
        cv::split(yuv, planes);
        REQUIRE(r->at<uint8_t>(0, 0) == planes[0].at<uint8_t>(0, 0));
    }
}

// -----------------------------------------------------------------------------
// Misc helpers.
// -----------------------------------------------------------------------------

TEST_CASE("ChannelView: checkerboard tile scales with image size",
          "[channel_view]") {
    // Small image: transparent pixel shows checker color (tile >= 8).
    cv::Mat small(16, 16, CV_8UC4, cv::Vec4b(0, 0, 0, 0));
    auto s = extract_channel_view(small, ChannelViewMode::None,
                                  ViewBackground::DarkChecker);
    REQUIRE(s.has_value());
    REQUIRE(s->at<cv::Vec4b>(0, 0)[0] > 0);

    // 4K image: tile must be large enough that the checker is visible.
    cv::Mat big(3840, 3840, CV_8UC4, cv::Vec4b(0, 0, 0, 0));
    auto b = extract_channel_view(big, ChannelViewMode::None,
                                  ViewBackground::DarkChecker);
    REQUIRE(b.has_value());
    uint8_t c0 = b->at<cv::Vec4b>(0, 0)[0];
    bool found_flip = false;
    for (int x = 8; x <= 256; x <<= 1) {
        if (b->at<cv::Vec4b>(0, x)[0] != c0) {
            found_flip = true;
            break;
        }
    }
    REQUIRE(found_flip);
}

TEST_CASE("ChannelView: no-alpha placeholder geometry", "[channel_view]") {
    // The placeholder is generated when alpha-only modes are requested on RGB.
    cv::Mat rgb = make_rgb8(4, 4, cv::Vec3b(100, 150, 200));
    REQUIRE_FALSE(
        extract_channel_view(rgb, ChannelViewMode::AlphaGray,
                             ViewBackground::Black)
            .has_value());

    auto placeholder = make_no_alpha_placeholder(64, 64);
    REQUIRE(placeholder.cols == 64);
    REQUIRE(placeholder.rows == 64);
    REQUIRE(placeholder.channels() == 4);

    cv::Vec4b off_diag = placeholder.at<cv::Vec4b>(10, 50);
    REQUIRE(off_diag == cv::Vec4b(64, 64, 64, 255));

    cv::Vec4b diag = placeholder.at<cv::Vec4b>(0, 0);
    REQUIRE(diag[2] == 255);
}
