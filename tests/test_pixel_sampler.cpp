#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "app/pixel_sampler.h"
#include "core/image.h"
#include "core/image_impl.h"

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

        // channel mismatch (1 vs 3: not the RGB<->RGBA bridge)
        b.depth = CV_8U;
        b.channels = 1;
        a.channels = 3;
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
    // 1 vs 3 has no defensible per-channel alignment, so it must stay
    // an em-dash.  (The 3 vs 4 case is intentionally bridged for
    // RGB <-> RGBA -- see the dedicated test further down.)
    PixelSample a; a.valid = true; a.channels = 3; a.depth = CV_8U;
    PixelSample b; b.valid = true; b.channels = 1; b.depth = CV_8U;
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

// -----------------------------------------------------------------------------
// Channel-kind labelling.
//
// The (a, b, c) triple printed in the inspector and status bar is
// ambiguous on its own -- users could not tell RGB from YUV at a
// glance.  format_pixel / format_delta therefore prepend a short
// channel-name prefix when the sample carries a known PixelKind, and
// leave the output prefix-free when kind is Unknown so all the
// pre-existing "(10, 20, 30)" expectations above keep passing.
// -----------------------------------------------------------------------------

TEST_CASE("format_pixel: RGB triple is prefixed with R G B",
          "[pixel_sampler][kind]") {
    PixelSample s;
    s.valid = true; s.channels = 3; s.depth = CV_8U;
    s.kind = PixelKind::RGB;
    s.v[0] = 255; s.v[1] = 0; s.v[2] = 0;
    char buf[64] = {};
    REQUIRE(format_pixel(s, buf, sizeof(buf)));
    REQUIRE(std::string(buf) == "R G B: (255, 0, 0)");
}

TEST_CASE("format_pixel: YUV triple is prefixed with Y Cb Cr",
          "[pixel_sampler][kind]") {
    PixelSample s;
    s.valid = true; s.channels = 3; s.depth = CV_8U;
    s.kind = PixelKind::YUV;
    s.v[0] = 180; s.v[1] = 128; s.v[2] = 128;
    char buf[64] = {};
    REQUIRE(format_pixel(s, buf, sizeof(buf)));
    REQUIRE(std::string(buf) == "Y Cb Cr: (180, 128, 128)");
}

TEST_CASE("format_pixel: gray scalar is prefixed with Y",
          "[pixel_sampler][kind]") {
    PixelSample s;
    s.valid = true; s.channels = 1; s.depth = CV_8U;
    s.kind = PixelKind::Gray;
    s.v[0] = 42;
    char buf[32] = {};
    REQUIRE(format_pixel(s, buf, sizeof(buf)));
    REQUIRE(std::string(buf) == "Y: 42");
}

TEST_CASE("format_pixel: RGBA quadruple is prefixed with R G B A",
          "[pixel_sampler][kind]") {
    PixelSample s;
    s.valid = true; s.channels = 4; s.depth = CV_8U;
    s.kind = PixelKind::RGB;
    s.v[0] = 1; s.v[1] = 2; s.v[2] = 3; s.v[3] = 4;
    char buf[64] = {};
    REQUIRE(format_pixel(s, buf, sizeof(buf)));
    REQUIRE(std::string(buf) == "R G B A: (1, 2, 3, 4)");
}

TEST_CASE("format_pixel: Unknown kind keeps the legacy no-prefix output",
          "[pixel_sampler][kind]") {
    // Pin the back-compat contract that the pre-existing tests above
    // implicitly rely on: a PixelSample with kind=Unknown must not
    // gain a prefix, otherwise older inspector / status-bar call sites
    // built before the labelling work would suddenly start emitting
    // "R G B:" for samples whose layout we do not actually know.
    PixelSample s;
    s.valid = true; s.channels = 3; s.depth = CV_8U;
    s.kind = PixelKind::Unknown;
    s.v[0] = 10; s.v[1] = 20; s.v[2] = 30;
    char buf[64] = {};
    REQUIRE(format_pixel(s, buf, sizeof(buf)));
    REQUIRE(std::string(buf) == "(10, 20, 30)");
}

TEST_CASE("format_delta: kind label inherits from the operands",
          "[pixel_sampler][kind]") {
    PixelSample a; a.valid = true; a.channels = 3; a.depth = CV_8U;
    a.kind = PixelKind::YUV;
    a.v[0] = 200; a.v[1] = 130; a.v[2] = 120;
    PixelSample b; b.valid = true; b.channels = 3; b.depth = CV_8U;
    b.kind = PixelKind::YUV;
    b.v[0] = 180; b.v[1] = 128; b.v[2] = 128;
    char buf[64] = {};
    REQUIRE(format_delta(a, b, buf, sizeof(buf)));
    REQUIRE(std::string(buf) == "Y Cb Cr: (+20, +2, -8)");
}

TEST_CASE("format_delta: mismatched kinds are not comparable",
          "[pixel_sampler][kind]") {
    // Cross-layout deltas (one side YUV, one side RGB) are numerically
    // meaningless: subtracting an R channel from a Y channel produces
    // a signed integer with no physical interpretation, and printing
    // it would mislead the reader far more than refusing to print it.
    // Treat the pair the same way as a depth or channel-count
    // mismatch -- emit an em-dash and report false -- so the caller
    // (and the user) get a clear "these two are not comparable"
    // signal instead of fake numbers.
    PixelSample a; a.valid = true; a.channels = 3; a.depth = CV_8U;
    a.kind = PixelKind::RGB;
    a.v[0] = 10; a.v[1] = 20; a.v[2] = 30;
    PixelSample b; b.valid = true; b.channels = 3; b.depth = CV_8U;
    b.kind = PixelKind::YUV;
    b.v[0] = 5;  b.v[1] = 18; b.v[2] = 32;
    char buf[64] = {};
    REQUIRE_FALSE(format_delta(a, b, buf, sizeof(buf)));
    REQUIRE(std::string(buf) == "\xE2\x80\x94");
}

TEST_CASE("format_delta: Unknown kind on one side is still allowed",
          "[pixel_sampler][kind]") {
    // Older inspector / status-bar code paths predating PixelKind, and
    // the existing back-compat tests above, build PixelSamples with
    // kind=Unknown.  Pair them with a kind-tagged sample and the
    // delta should still go through (using the tagged side's prefix);
    // only a *known* kind mismatch is rejected.
    PixelSample a; a.valid = true; a.channels = 3; a.depth = CV_8U;
    a.kind = PixelKind::RGB;
    a.v[0] = 10; a.v[1] = 20; a.v[2] = 30;
    PixelSample b; b.valid = true; b.channels = 3; b.depth = CV_8U;
    // b.kind defaults to Unknown.
    b.v[0] = 5;  b.v[1] = 18; b.v[2] = 32;
    char buf[64] = {};
    REQUIRE(format_delta(a, b, buf, sizeof(buf)));
    REQUIRE(std::string(buf) == "R G B: (+5, +2, -2)");
}

// -----------------------------------------------------------------------------
// sample_image_at: prefer_rgb gate.
//
// The PixelInspectorPanel exposes a YUV/RGB radio for video sources;
// internally that radio toggles the prefer_rgb argument.  We cannot
// build an AVFrame in a unit test without dragging in the whole video
// stack, but we can pin down the still-image / mat-fallback behaviour:
// regardless of prefer_rgb, an Image with no AVFrame attached must
// produce the exact same RGB-tagged sample.
// -----------------------------------------------------------------------------

TEST_CASE("sample_image_at: prefer_rgb is a no-op when there is no AVFrame",
          "[pixel_sampler][kind]") {
    // Build an Image-shaped object via a small synthetic mat -- no
    // FFmpeg involved.  The mat-fallback path tags the result as RGB
    // because that is the only layout the SDL-domain mat ever holds
    // in this codebase.
    cv::Mat m(2, 2, CV_8UC3, cv::Scalar(10, 20, 30));
    Image img;
    img.internal().mat = m;

    auto a = sample_image_at(&img, 0.5, 0.5, /*prefer_rgb=*/false);
    auto b = sample_image_at(&img, 0.5, 0.5, /*prefer_rgb=*/true);

    REQUIRE(a.valid);
    REQUIRE(b.valid);
    REQUIRE(a.kind == PixelKind::RGB);
    REQUIRE(b.kind == PixelKind::RGB);
    REQUIRE(a.channels == b.channels);
    for (int i = 0; i < a.channels; ++i) {
        REQUIRE(a.v[i] == b.v[i]);
    }
}

// -----------------------------------------------------------------------------
// format_delta: RGB <-> RGBA bridge.
//
// Real-world trigger: a user lines up an RGB24 PNG against a PAL8
// PNG that decodes (via tRNS or the OpenCV / ImageMagick alpha
// expansion) to RGBA8.  Refusing to subtract the two would leave the
// Delta column permanently empty, which is the exact regression we
// want to fix.  Treat the missing alpha as fully opaque so the user
// sees how far the RGBA side is from opaque, and tag the row with
// the wider "R G B A:" prefix so the channel geometry stays obvious.
// -----------------------------------------------------------------------------

TEST_CASE("format_delta: RGB(3) vs RGBA(4) bridges with opaque alpha",
          "[pixel_sampler][kind]") {
    PixelSample rgb;
    rgb.valid = true; rgb.channels = 3; rgb.depth = CV_8U;
    rgb.kind = PixelKind::RGB;
    rgb.v[0] = 100; rgb.v[1] = 150; rgb.v[2] = 200;

    PixelSample rgba;
    rgba.valid = true; rgba.channels = 4; rgba.depth = CV_8U;
    rgba.kind = PixelKind::RGB;
    rgba.v[0] = 90; rgba.v[1] = 160; rgba.v[2] = 200; rgba.v[3] = 128;

    char buf[96] = {};

    SECTION("rgb minus rgba: missing alpha back-fills as 255") {
        // 255 (rgb) - 128 (rgba) = +127 in the alpha column.
        REQUIRE(format_delta(rgb, rgba, buf, sizeof(buf)));
        REQUIRE(std::string(buf) == "R G B A: (+10, -10, 0, +127)");
    }

    SECTION("rgba minus rgb: symmetric, opposite sign on alpha") {
        REQUIRE(format_delta(rgba, rgb, buf, sizeof(buf)));
        REQUIRE(std::string(buf) == "R G B A: (-10, +10, 0, -127)");
    }
}

TEST_CASE("format_delta: RGB <-> RGBA bridge works across supported depths",
          "[pixel_sampler][kind]") {
    SECTION("16-bit: opaque alpha is 65535") {
        PixelSample rgb;
        rgb.valid = true; rgb.channels = 3; rgb.depth = CV_16U;
        rgb.kind = PixelKind::RGB;
        rgb.v[0] = 1000; rgb.v[1] = 2000; rgb.v[2] = 3000;

        PixelSample rgba;
        rgba.valid = true; rgba.channels = 4; rgba.depth = CV_16U;
        rgba.kind = PixelKind::RGB;
        rgba.v[0] = 1000; rgba.v[1] = 2000; rgba.v[2] = 3000;
        rgba.v[3] = 32768;

        char buf[96] = {};
        REQUIRE(format_delta(rgb, rgba, buf, sizeof(buf)));
        REQUIRE(std::string(buf) == "R G B A: (0, 0, 0, +32767)");
    }

    SECTION("32F: opaque alpha is 1.0") {
        PixelSample rgb;
        rgb.valid = true; rgb.channels = 3; rgb.depth = CV_32F;
        rgb.kind = PixelKind::RGB;
        rgb.v[0] = 0.25; rgb.v[1] = 0.5; rgb.v[2] = 0.75;

        PixelSample rgba;
        rgba.valid = true; rgba.channels = 4; rgba.depth = CV_32F;
        rgba.kind = PixelKind::RGB;
        rgba.v[0] = 0.25; rgba.v[1] = 0.5; rgba.v[2] = 0.75;
        rgba.v[3] = 0.5;

        char buf[96] = {};
        REQUIRE(format_delta(rgb, rgba, buf, sizeof(buf)));
        REQUIRE(std::string(buf) == "R G B A: (0.0000, 0.0000, 0.0000, +0.5000)");
    }
}

TEST_CASE("format_delta: RGB <-> RGBA bridge requires RGB-shaped layout",
          "[pixel_sampler][kind]") {
    // A 3-channel YUV frame and a 4-channel "RGBA" sample have no
    // shared interpretation -- alpha back-fill would be meaningless
    // because the 3-channel side is not even RGB.  The bridge must
    // refuse this pair the same way it refuses any cross-kind delta.
    PixelSample yuv;
    yuv.valid = true; yuv.channels = 3; yuv.depth = CV_8U;
    yuv.kind = PixelKind::YUV;
    yuv.v[0] = 180; yuv.v[1] = 128; yuv.v[2] = 128;

    PixelSample rgba;
    rgba.valid = true; rgba.channels = 4; rgba.depth = CV_8U;
    rgba.kind = PixelKind::RGB;
    rgba.v[0] = 100; rgba.v[1] = 100; rgba.v[2] = 100; rgba.v[3] = 255;

    char buf[64] = {};
    REQUIRE_FALSE(format_delta(yuv, rgba, buf, sizeof(buf)));
    REQUIRE(std::string(buf) == "\xE2\x80\x94");
}

TEST_CASE("format_delta: RGB <-> RGBA bridge accepts Unknown-kind operands",
          "[pixel_sampler][kind]") {
    // Pre-PixelKind call sites build samples with kind=Unknown.  The
    // bridge should still trigger so the long-standing contract that
    // Unknown is treated as "trust the caller" carries over to the
    // 3-vs-4 case as well.
    PixelSample three;
    three.valid = true; three.channels = 3; three.depth = CV_8U;
    three.v[0] = 10; three.v[1] = 20; three.v[2] = 30;

    PixelSample four;
    four.valid = true; four.channels = 4; four.depth = CV_8U;
    four.v[0] = 10; four.v[1] = 20; four.v[2] = 30; four.v[3] = 200;

    char buf[96] = {};
    REQUIRE(format_delta(three, four, buf, sizeof(buf)));
    // No prefix because both sides are Unknown -- legacy back-compat.
    REQUIRE(std::string(buf) == "(0, 0, 0, +55)");
}
