#include <catch2/catch_test_macros.hpp>

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
std::string write_tmp_yuv(const std::string& tag,
                          const YuvStreamParams& p,
                          int frame_count,
                          std::size_t& frame_bytes_out) {
    frame_bytes_out = yuv_frame_size_bytes(p);
    REQUIRE(frame_bytes_out > 0);

    auto dir = std::filesystem::temp_directory_path();
    auto path = dir / (std::string("idiff_yuv_") + tag + ".yuv");

    std::vector<uint8_t> frame(frame_bytes_out);
    // Y = 128, U = 128, V = 128 -> a mid-gray frame.  Using a flat grey
    // makes the resulting RGB deterministic and decode-correctness easy
    // to assert.  We use a known per-frame byte pattern by including the
    // frame index so read_frame(idx) can be validated against a clamp.
    std::fill(frame.begin(), frame.end(), static_cast<uint8_t>(128));

    std::ofstream out(path, std::ios::binary);
    REQUIRE(out.is_open());
    for (int i = 0; i < frame_count; i++) {
        // First byte of each frame = frame index mod 256, so decoded
        // top-left pixel encodes the frame index.  This lets the test
        // verify that read_frame(i) actually read frame i.
        frame[0] = static_cast<uint8_t>(i & 0xFF);
        out.write(reinterpret_cast<const char*>(frame.data()), frame.size());
    }
    out.close();
    return path.string();
}

} // namespace

TEST_CASE("yuv_frame_size_bytes: correct sizes for supported formats", "[yuv]")
{
    YuvStreamParams p;
    p.width = 1920;
    p.height = 1080;

    p.pixel_format = YuvPixelFormat::YUV420P;
    REQUIRE(yuv_frame_size_bytes(p) == 1920ULL * 1080 * 3 / 2);

    p.pixel_format = YuvPixelFormat::YUV422P;
    REQUIRE(yuv_frame_size_bytes(p) == 1920ULL * 1080 * 2);

    p.pixel_format = YuvPixelFormat::YUV444P;
    REQUIRE(yuv_frame_size_bytes(p) == 1920ULL * 1080 * 3);
}

TEST_CASE("yuv_frame_size_bytes: rejects invalid sizes", "[yuv]")
{
    YuvStreamParams p;
    // Zero dimensions
    REQUIRE(yuv_frame_size_bytes(p) == 0);

    // Odd height for 4:2:0 is invalid
    p.width = 64; p.height = 63;
    p.pixel_format = YuvPixelFormat::YUV420P;
    REQUIRE(yuv_frame_size_bytes(p) == 0);

    // Odd width for 4:2:2 is invalid
    p.width = 63; p.height = 64;
    p.pixel_format = YuvPixelFormat::YUV422P;
    REQUIRE(yuv_frame_size_bytes(p) == 0);

    // 4:4:4 accepts any positive size.
    p.pixel_format = YuvPixelFormat::YUV444P;
    REQUIRE(yuv_frame_size_bytes(p) == 63ULL * 64 * 3);
}

TEST_CASE("guess_yuv_params_from_filename: resolution", "[yuv]")
{
    YuvStreamParams p;
    bool changed = guess_yuv_params_from_filename("clip_1920x1080.yuv", p);
    REQUIRE(changed);
    REQUIRE(p.width == 1920);
    REQUIRE(p.height == 1080);
}

TEST_CASE("guess_yuv_params_from_filename: pixel format keywords", "[yuv]")
{
    {
        YuvStreamParams p;
        guess_yuv_params_from_filename("clip_yuv422p_640x480.yuv", p);
        REQUIRE(p.pixel_format == YuvPixelFormat::YUV422P);
        REQUIRE(p.width == 640);
        REQUIRE(p.height == 480);
    }
    {
        YuvStreamParams p;
        guess_yuv_params_from_filename("clip_yuv444p_640x480.yuv", p);
        REQUIRE(p.pixel_format == YuvPixelFormat::YUV444P);
    }
    {
        YuvStreamParams p;
        guess_yuv_params_from_filename("something_i420.yuv", p);
        REQUIRE(p.pixel_format == YuvPixelFormat::YUV420P);
    }
}

TEST_CASE("guess_yuv_params_from_filename: unrecognized name leaves defaults",
          "[yuv]")
{
    YuvStreamParams p;
    p.width = 0;
    p.height = 0;
    bool changed = guess_yuv_params_from_filename("plain.yuv", p);
    REQUIRE_FALSE(changed);
    REQUIRE(p.width == 0);
    REQUIRE(p.height == 0);
}

TEST_CASE("YuvRawSource: frame count matches file size / frame size", "[yuv]")
{
    YuvStreamParams p;
    p.width = 16;
    p.height = 16;
    p.pixel_format = YuvPixelFormat::YUV420P;
    std::size_t fb = 0;
    std::string path = write_tmp_yuv("count", p, 5, fb);

    YuvRawSource src(path, p);
    REQUIRE(src.frame_count() == 5);
    REQUIRE(src.width() == 16);
    REQUIRE(src.height() == 16);
    REQUIRE_FALSE(src.format_description().empty());

    std::filesystem::remove(path);
}

TEST_CASE("YuvRawSource: read_frame returns RGB Image of the right size", "[yuv]")
{
    YuvStreamParams p;
    p.width = 16;
    p.height = 16;
    p.pixel_format = YuvPixelFormat::YUV420P;
    std::size_t fb = 0;
    std::string path = write_tmp_yuv("decode", p, 3, fb);

    YuvRawSource src(path, p);
    auto img = src.read_frame(0);
    REQUIRE(img != nullptr);
    REQUIRE(img->info().width == 16);
    REQUIRE(img->info().height == 16);
    REQUIRE(img->info().bit_depth == 8);
    REQUIRE(img->info().has_alpha == false);
    REQUIRE(img->mat().channels() == 3);

    std::filesystem::remove(path);
}

TEST_CASE("YuvRawSource: out-of-range frame is rejected", "[yuv]")
{
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

TEST_CASE("YuvRawSource: invalid parameters reported via frame_count == 0",
          "[yuv]")
{
    YuvStreamParams p;
    p.width = 0;
    p.height = 0;
    p.pixel_format = YuvPixelFormat::YUV420P;

    YuvRawSource src("/tmp/does_not_matter.yuv", p);
    REQUIRE(src.frame_count() == 0);
    REQUIRE(src.read_frame(0) == nullptr);
}

TEST_CASE("YuvRawSource: works for all three supported pixel formats", "[yuv]")
{
    for (auto fmt : { YuvPixelFormat::YUV420P,
                      YuvPixelFormat::YUV422P,
                      YuvPixelFormat::YUV444P }) {
        YuvStreamParams p;
        p.width = 16;
        p.height = 16;
        p.pixel_format = fmt;
        std::size_t fb = 0;
        std::string path = write_tmp_yuv("fmt", p, 1, fb);

        YuvRawSource src(path, p);
        auto img = src.read_frame(0);
        REQUIRE(img != nullptr);
        REQUIRE(img->info().width == 16);
        REQUIRE(img->info().height == 16);

        std::filesystem::remove(path);
    }
}
