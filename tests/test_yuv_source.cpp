#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "core/media_source.h"
#include "core/image.h"

using namespace idiff;

namespace {

// Build a synthetic planar YUV frame with distinct per-plane constants
// and write `frame_count` copies to a temp file.  Returns the path and
// sets frame_bytes to the size of one frame.
std::string write_tmp_yuv(const std::string& tag, const YuvStreamParams& p,
                          int frame_count, std::size_t& frame_bytes_out) {
    frame_bytes_out = yuv_frame_size_bytes(p);
    REQUIRE(frame_bytes_out > 0);

    auto dir = std::filesystem::temp_directory_path();
    auto path = dir / (std::string("idiff_yuv_") + tag + ".yuv");

    std::vector<uint8_t> frame(frame_bytes_out);
    std::fill(frame.begin(), frame.end(), static_cast<uint8_t>(128));

    std::ofstream out(path, std::ios::binary);
    REQUIRE(out.is_open());
    for (int i = 0; i < frame_count; i++) {
        // First byte of each frame = frame index mod 256, so decoded
        // top-left pixel encodes the frame index.
        frame[0] = static_cast<uint8_t>(i & 0xFF);
        out.write(reinterpret_cast<const char*>(frame.data()), frame.size());
    }
    out.close();
    return path.string();
}

}  // namespace

// -----------------------------------------------------------------------------
// yuv_frame_size_bytes.
// -----------------------------------------------------------------------------

TEST_CASE("yuv_frame_size_bytes computes sizes and rejects bad shapes",
          "[yuv]") {
    YuvStreamParams p;

    SECTION("supported formats produce expected byte totals") {
        p.width = 1920;
        p.height = 1080;
        p.pixel_format = YuvPixelFormat::YUV420P;
        REQUIRE(yuv_frame_size_bytes(p) == 1920ULL * 1080 * 3 / 2);
        p.pixel_format = YuvPixelFormat::YUV422P;
        REQUIRE(yuv_frame_size_bytes(p) == 1920ULL * 1080 * 2);
        p.pixel_format = YuvPixelFormat::YUV444P;
        REQUIRE(yuv_frame_size_bytes(p) == 1920ULL * 1080 * 3);
    }

    SECTION("zero-dimension input is rejected") {
        REQUIRE(yuv_frame_size_bytes(p) == 0);
    }

    SECTION("420P rejects odd height, 422P rejects odd width") {
        p.width = 64;
        p.height = 63;
        p.pixel_format = YuvPixelFormat::YUV420P;
        REQUIRE(yuv_frame_size_bytes(p) == 0);

        p.width = 63;
        p.height = 64;
        p.pixel_format = YuvPixelFormat::YUV422P;
        REQUIRE(yuv_frame_size_bytes(p) == 0);
    }

    SECTION("444P accepts any positive size") {
        p.width = 63;
        p.height = 64;
        p.pixel_format = YuvPixelFormat::YUV444P;
        REQUIRE(yuv_frame_size_bytes(p) == 63ULL * 64 * 3);
    }
}

// -----------------------------------------------------------------------------
// guess_yuv_params_from_filename.
// -----------------------------------------------------------------------------

TEST_CASE("guess_yuv_params_from_filename parses size and format hints",
          "[yuv]") {
    SECTION("WIDTHxHEIGHT token populates dimensions") {
        YuvStreamParams p;
        REQUIRE(guess_yuv_params_from_filename("clip_1920x1080.yuv", p));
        REQUIRE(p.width == 1920);
        REQUIRE(p.height == 1080);
    }
    SECTION("explicit yuv422p / yuv444p / i420 keywords map correctly") {
        YuvStreamParams p;
        guess_yuv_params_from_filename("clip_yuv422p_640x480.yuv", p);
        REQUIRE(p.pixel_format == YuvPixelFormat::YUV422P);
        REQUIRE(p.width == 640);
        REQUIRE(p.height == 480);

        YuvStreamParams q;
        guess_yuv_params_from_filename("clip_yuv444p_640x480.yuv", q);
        REQUIRE(q.pixel_format == YuvPixelFormat::YUV444P);

        YuvStreamParams r;
        guess_yuv_params_from_filename("something_i420.yuv", r);
        REQUIRE(r.pixel_format == YuvPixelFormat::YUV420P);
    }
    SECTION("unrecognized filename leaves the params untouched") {
        YuvStreamParams p;
        p.width = 0;
        p.height = 0;
        REQUIRE_FALSE(guess_yuv_params_from_filename("plain.yuv", p));
        REQUIRE(p.width == 0);
        REQUIRE(p.height == 0);
    }
}

// -----------------------------------------------------------------------------
// YuvRawSource happy path.
// -----------------------------------------------------------------------------

TEST_CASE("YuvRawSource decodes every supported pixel format", "[yuv]") {
    auto fmt = GENERATE(YuvPixelFormat::YUV420P, YuvPixelFormat::YUV422P,
                        YuvPixelFormat::YUV444P);
    CAPTURE(static_cast<int>(fmt));

    YuvStreamParams p;
    p.width = 16;
    p.height = 16;
    p.pixel_format = fmt;
    std::size_t fb = 0;
    std::string path = write_tmp_yuv("fmt", p, 5, fb);

    YuvRawSource src(path, p);
    REQUIRE(src.frame_count() == 5);
    REQUIRE(src.width() == 16);
    REQUIRE(src.height() == 16);
    REQUIRE_FALSE(src.format_description().empty());

    auto img = src.read_frame(0);
    REQUIRE(img != nullptr);
    REQUIRE(img->info().width == 16);
    REQUIRE(img->info().height == 16);
    REQUIRE(img->info().bit_depth == 8);
    REQUIRE(img->info().has_alpha == false);
    REQUIRE(img->mat().channels() == 3);

    std::filesystem::remove(path);
}

// -----------------------------------------------------------------------------
// YuvRawSource error paths.
// -----------------------------------------------------------------------------

TEST_CASE("YuvRawSource: rejects invalid frame indices and invalid params",
          "[yuv]") {
    SECTION("out-of-range and negative frame indices") {
        YuvStreamParams p;
        p.width = 16;
        p.height = 16;
        p.pixel_format = YuvPixelFormat::YUV420P;
        std::size_t fb = 0;
        std::string path = write_tmp_yuv("oob", p, 2, fb);

        YuvRawSource src(path, p);
        REQUIRE(src.read_frame(2) == nullptr);   // index == count
        REQUIRE(src.read_frame(-1) == nullptr);  // negative
        REQUIRE_FALSE(src.last_error().empty());

        std::filesystem::remove(path);
    }

    SECTION("invalid params surface as frame_count == 0") {
        YuvStreamParams p;
        p.width = 0;
        p.height = 0;
        p.pixel_format = YuvPixelFormat::YUV420P;

        YuvRawSource src("/tmp/does_not_matter.yuv", p);
        REQUIRE(src.frame_count() == 0);
        REQUIRE(src.read_frame(0) == nullptr);
    }
}
