// Tests for animated GIF support.  The classifier tests run in every
// build configuration; the decoder tests are gated on the backend that
// actually handles GIFs in that configuration.

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
#include <string>

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
