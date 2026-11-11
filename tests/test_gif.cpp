// Tests for animated GIF support.  The classifier tests run in every
// build configuration; the decoder tests are gated on the backend that
// actually handles GIFs in that configuration.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/image.h"
#include "core/image_impl.h"
#include "core/media_source.h"

#include <opencv2/core.hpp>

#ifdef IDIFF_HAVE_FFMPEG
#include "core/video_file_source.h"
#endif
#if defined(IDIFF_HAVE_MAGICK) && !defined(IDIFF_HAVE_FFMPEG)
#include "core/gif_magick_source.h"
#endif

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifndef IDIFF_TEST_GIF_PATH
#define IDIFF_TEST_GIF_PATH ""
#endif

using namespace idiff;

namespace {

bool has_gif_fixture() {
    return std::string(IDIFF_TEST_GIF_PATH).size() > 0;
}

bool same_pixels(const cv::Mat& a, const cv::Mat& b) {
    if (a.size() != b.size() || a.type() != b.type()) return false;
    return std::memcmp(a.data, b.data,
                       a.total() * a.elemSize()) == 0;
}

// Two 1x1 frames, each optionally preceded by a Graphic Control
// Extension whose delay field is 0.  A general-purpose GIF writer
// always emits some delay, so the bytes are literal.  `with_gce` picks
// the two shapes that differ in how libavformat reads them: a frame
// with no extension carries no timing at all, while an explicit zero
// delay is replaced with 0.1 s.
std::string write_two_frame_gif(bool with_gce) {
    std::vector<unsigned char> gif = {
        0x47, 0x49, 0x46, 0x38, 0x39, 0x61,             // "GIF89a"
        0x01, 0x00, 0x01, 0x00, 0xF0, 0x00, 0x00,       // logical screen
        0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF,             // global colour table
    };
    static const unsigned char kGce[] = {
        0x21, 0xF9, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, // delay = 0
    };
    static const unsigned char kFrame[] = {
        0x2C, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01,
        0x00, 0x00, 0x02, 0x02, 0x44, 0x01, 0x00,
    };
    for (int i = 0; i < 2; ++i) {
        if (with_gce) {
            gif.insert(gif.end(), std::begin(kGce), std::end(kGce));
        }
        gif.insert(gif.end(), std::begin(kFrame), std::end(kFrame));
    }
    gif.push_back(0x3B);                                // trailer

    const auto path = std::filesystem::temp_directory_path()
                    / (with_gce ? "idiff_zero_delay.gif"
                                : "idiff_timingless.gif");
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(gif.data()),
              static_cast<std::streamsize>(gif.size()));
    out.close();
    return path.string();
}

} // namespace

TEST_CASE("is_gif_extension accepts only the .gif extension", "[gif]") {
    CHECK(is_gif_extension("anim.gif"));
    CHECK(is_gif_extension("ANIM.GIF"));
    CHECK(is_gif_extension("/tmp/a/b.GiF"));
    CHECK_FALSE(is_gif_extension("anim.png"));
    CHECK_FALSE(is_gif_extension("a.gifx"));
    CHECK_FALSE(is_gif_extension("gif"));
    CHECK_FALSE(is_gif_extension(""));
}

#ifdef IDIFF_HAVE_FFMPEG

TEST_CASE("GIF is routed through the video container classifier",
          "[gif][video]") {
    CHECK(is_video_file_extension("anim.gif"));
    CHECK(is_video_file_extension("clip.GIF"));
}

TEST_CASE("VideoFileSource exposes every frame of an animated GIF",
          "[gif][video]") {
    if (!has_gif_fixture()) SKIP("GIF fixture not generated");

    VideoFileSource src(IDIFF_TEST_GIF_PATH);
    REQUIRE(src.is_valid());
    REQUIRE(src.frame_count() == 5);
    CHECK(src.width() == 64);
    CHECK(src.height() == 64);

    // The fixture holds five frames at 0.2 s each.  The stream's
    // avg_frame_rate must reach the timeline instead of a fixed default.
    CHECK(src.frame_rate() == Catch::Approx(5.0));

    auto f0 = src.read_frame(0);
    auto f1 = src.read_frame(1);
    REQUIRE(f0);
    REQUIRE(f1);
    CHECK(f0->info().source_format == SourceFormat::GIF);
    CHECK_FALSE(same_pixels(f0->internal().mat, f1->internal().mat));
    CHECK(src.read_frame(-1) == nullptr);
    CHECK(src.read_frame(5) == nullptr);

    // A GIF marks only frame 0 as a keyframe.  The approximate scrub
    // path must still return the exact requested frame rather than
    // pinning every preview to the first one.
    auto k2 = src.read_keyframe(2);
    auto f2 = src.read_frame(2);
    REQUIRE(k2);
    REQUIRE(f2);
    CHECK(same_pixels(k2->internal().mat, f2->internal().mat));
}

#endif // IDIFF_HAVE_FFMPEG

#if defined(IDIFF_HAVE_MAGICK) && !defined(IDIFF_HAVE_FFMPEG)

TEST_CASE("GifMagickSource exposes coalesced frames without FFmpeg",
          "[gif][magick]") {
    if (!has_gif_fixture()) SKIP("GIF fixture not generated");

    GifMagickSource src(IDIFF_TEST_GIF_PATH);
    REQUIRE(src.is_valid());
    REQUIRE(src.frame_count() == 5);
    CHECK(src.width() == 64);
    CHECK(src.height() == 64);
    // Five frames with a 20/100 s delay each average to 5 fps.
    CHECK(src.frame_rate() == Catch::Approx(5.0));

    auto f0 = src.read_frame(0);
    auto f1 = src.read_frame(1);
    REQUIRE(f0);
    REQUIRE(f1);
    CHECK(f0->info().source_format == SourceFormat::GIF);
    CHECK_FALSE(same_pixels(f0->internal().mat, f1->internal().mat));
    CHECK(src.read_frame(-1) == nullptr);
    CHECK(src.read_frame(5) == nullptr);
}

#endif // IDIFF_HAVE_MAGICK && !IDIFF_HAVE_FFMPEG

#if defined(IDIFF_HAVE_FFMPEG) || defined(IDIFF_HAVE_MAGICK)

TEST_CASE("GIF frames with no delay resolve per backend", "[gif]") {
    // No Graphic Control Extension anywhere.  libavformat sees zero
    // duration and does not trust the time-base-derived r_frame_rate,
    // so there is no rate.  Magick++ cannot tell this shape from an
    // explicit zero delay and applies its 0.1 s substitution.
    const std::string path = write_two_frame_gif(false);
#if defined(IDIFF_HAVE_FFMPEG)
    VideoFileSource src(path);
    REQUIRE(src.is_valid());
    CHECK(src.frame_count() == 2);
    CHECK(src.frame_rate() == 0.0);
#else
    GifMagickSource src(path);
    REQUIRE(src.is_valid());
    CHECK(src.frame_count() == 2);
    CHECK(src.frame_rate() == Catch::Approx(10.0));
#endif
}

TEST_CASE("A zero-delay GCE reports 10 fps in both backends", "[gif]") {
    // libavformat replaces an explicit zero delay with 0.1 s, and
    // Magick++ applies the same substitution, so this shape must agree
    // across build configurations.
    const std::string path = write_two_frame_gif(true);
#if defined(IDIFF_HAVE_FFMPEG)
    VideoFileSource src(path);
    REQUIRE(src.is_valid());
    CHECK(src.frame_count() == 2);
    CHECK(src.frame_rate() == Catch::Approx(10.0));
#else
    GifMagickSource src(path);
    REQUIRE(src.is_valid());
    CHECK(src.frame_count() == 2);
    CHECK(src.frame_rate() == Catch::Approx(10.0));
#endif
}

#endif // IDIFF_HAVE_FFMPEG || IDIFF_HAVE_MAGICK
