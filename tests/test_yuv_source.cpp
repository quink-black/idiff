#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/core.hpp>

#include "core/media_source.h"
#include "core/image.h"
#include "core/image_impl.h"
#ifdef IDIFF_HAVE_FFMPEG
#include "core/yuv_raw_source.h"
#endif

using namespace idiff;

namespace {

// RAII helper to remove a temp file.  On Windows, FFmpeg may still
// hold the file handle when std::filesystem::remove() is first called.
// This helper retries a few times before giving up.
struct TempFile {
    std::filesystem::path path;
    explicit TempFile(std::filesystem::path p) : path(std::move(p)) {}
    ~TempFile() {
        std::error_code ec;
        for (int i = 0; i < 10; ++i) {
            if (std::filesystem::remove(path, ec) || ec) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
};

// Build a synthetic planar 8-bit YUV frame with distinct per-plane constants
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

// Build a synthetic 10-bit planar YUV frame (16-bit LE samples) and write
// `frame_count` copies to a temp file.  Each sample is set to `value10`
// (stored in the lower 10 bits of a 16-bit LE word).
std::string write_tmp_yuv_10bit(const std::string& tag, const YuvStreamParams& p,
                                int frame_count, uint16_t value10,
                                std::size_t& frame_bytes_out) {
    frame_bytes_out = yuv_frame_size_bytes(p);
    REQUIRE(frame_bytes_out > 0);

    auto dir = std::filesystem::temp_directory_path();
    auto path = dir / (std::string("idiff_yuv10_") + tag + ".yuv");

    // value10 in the lower 10 bits, stored as 16-bit LE.
    uint16_t val = value10 & 0x3FF;
    uint8_t lo = static_cast<uint8_t>(val & 0xFF);
    uint8_t hi = static_cast<uint8_t>((val >> 8) & 0xFF);

    // Alternate lo/hi bytes for the entire frame.
    std::vector<uint8_t> frame(frame_bytes_out);
    for (std::size_t i = 0; i + 1 < frame_bytes_out; i += 2) {
        frame[i] = lo;
        frame[i + 1] = hi;
    }
    // First two bytes encode frame index in the low byte.
    for (int i = 0; i < frame_count; i++) {
        frame[0] = static_cast<uint8_t>(i & 0xFF);
        std::ofstream out(path, std::ios::binary | std::ios::app);
        REQUIRE(out.is_open());
        out.write(reinterpret_cast<const char*>(frame.data()), frame.size());
        out.close();
    }
    return path.string();
}

// Build a synthetic P010 frame (Y plane + interleaved UV plane, all 16-bit LE).
std::string write_tmp_p010(const std::string& tag, const YuvStreamParams& p,
                           int frame_count, uint16_t value10,
                           std::size_t& frame_bytes_out) {
    return write_tmp_yuv_10bit(tag, p, frame_count, value10, frame_bytes_out);
}

// Build a synthetic NV16 frame (Y plane + interleaved UV plane, 8-bit).
std::string write_tmp_nv16(const std::string& tag, const YuvStreamParams& p,
                           int frame_count, std::size_t& frame_bytes_out) {
    return write_tmp_yuv(tag, p, frame_count, frame_bytes_out);
}

}  // namespace

// -----------------------------------------------------------------------------
// yuv_frame_size_bytes.
// -----------------------------------------------------------------------------

TEST_CASE("yuv_frame_size_bytes computes sizes and rejects bad shapes",
          "[yuv]") {
    YuvStreamParams p;

    SECTION("8-bit planar formats produce expected byte totals") {
        p.width = 1920;
        p.height = 1080;
        p.pixel_format = YuvPixelFormat::YUV420P;
        REQUIRE(yuv_frame_size_bytes(p) == 1920ULL * 1080 * 3 / 2);
        p.pixel_format = YuvPixelFormat::YUV422P;
        REQUIRE(yuv_frame_size_bytes(p) == 1920ULL * 1080 * 2);
        p.pixel_format = YuvPixelFormat::YUV444P;
        REQUIRE(yuv_frame_size_bytes(p) == 1920ULL * 1080 * 3);
    }

    SECTION("10-bit planar formats produce expected byte totals") {
        p.width = 16;
        p.height = 16;
        p.pixel_format = YuvPixelFormat::YUV420P10;
        // Y: 16*16*2 = 512, U: 8*8*2 = 128, V: 8*8*2 = 128 => 768
        REQUIRE(yuv_frame_size_bytes(p) == 768);
        p.pixel_format = YuvPixelFormat::YUV422P10;
        // Y: 16*16*2 = 512, U: 8*16*2 = 256, V: 8*16*2 = 256 => 1024
        REQUIRE(yuv_frame_size_bytes(p) == 1024);
        p.pixel_format = YuvPixelFormat::YUV444P10;
        // Y+U+V: 16*16*2*3 = 1536
        REQUIRE(yuv_frame_size_bytes(p) == 1536);
    }

    SECTION("semi-planar formats produce expected byte totals") {
        p.width = 16;
        p.height = 16;
        p.pixel_format = YuvPixelFormat::P010;
        // Same as YUV420P10: Y*2 + UV*2 = 16*16*2 + 8*8*2*2 = 768
        REQUIRE(yuv_frame_size_bytes(p) == 768);
        p.pixel_format = YuvPixelFormat::NV16;
        // Y + UV interleaved = 16*16 + 16*16 = 512
        REQUIRE(yuv_frame_size_bytes(p) == 512);
    }

    SECTION("zero-dimension input is rejected") {
        REQUIRE(yuv_frame_size_bytes(p) == 0);
    }

    SECTION("420P and P010 reject odd height or width") {
        p.width = 64;
        p.height = 63;
        p.pixel_format = YuvPixelFormat::YUV420P;
        REQUIRE(yuv_frame_size_bytes(p) == 0);
        p.pixel_format = YuvPixelFormat::P010;
        REQUIRE(yuv_frame_size_bytes(p) == 0);

        p.width = 63;
        p.height = 64;
        p.pixel_format = YuvPixelFormat::YUV420P;
        REQUIRE(yuv_frame_size_bytes(p) == 0);
        p.pixel_format = YuvPixelFormat::P010;
        REQUIRE(yuv_frame_size_bytes(p) == 0);
    }

    SECTION("422P and NV16 reject odd width") {
        p.width = 63;
        p.height = 64;
        p.pixel_format = YuvPixelFormat::YUV422P;
        REQUIRE(yuv_frame_size_bytes(p) == 0);
        p.pixel_format = YuvPixelFormat::NV16;
        REQUIRE(yuv_frame_size_bytes(p) == 0);
    }

    SECTION("444P and 444P10 accept any positive size") {
        p.width = 63;
        p.height = 64;
        p.pixel_format = YuvPixelFormat::YUV444P;
        REQUIRE(yuv_frame_size_bytes(p) == 63ULL * 64 * 3);
        p.pixel_format = YuvPixelFormat::YUV444P10;
        REQUIRE(yuv_frame_size_bytes(p) == 63ULL * 64 * 2 * 3);
    }
}

// -----------------------------------------------------------------------------
// yuv_pixel_format_name and yuv_pixel_format_bit_depth.
// -----------------------------------------------------------------------------

TEST_CASE("yuv_pixel_format_name and bit_depth helpers", "[yuv]") {
    REQUIRE(std::string(yuv_pixel_format_name(YuvPixelFormat::YUV420P)) == "YUV420P");
    REQUIRE(std::string(yuv_pixel_format_name(YuvPixelFormat::YUV422P)) == "YUV422P");
    REQUIRE(std::string(yuv_pixel_format_name(YuvPixelFormat::YUV444P)) == "YUV444P");
    REQUIRE(std::string(yuv_pixel_format_name(YuvPixelFormat::YUV420P10)) == "YUV420P10");
    REQUIRE(std::string(yuv_pixel_format_name(YuvPixelFormat::YUV422P10)) == "YUV422P10");
    REQUIRE(std::string(yuv_pixel_format_name(YuvPixelFormat::YUV444P10)) == "YUV444P10");
    REQUIRE(std::string(yuv_pixel_format_name(YuvPixelFormat::P010)) == "P010");
    REQUIRE(std::string(yuv_pixel_format_name(YuvPixelFormat::NV16)) == "NV16");

    REQUIRE(yuv_pixel_format_bit_depth(YuvPixelFormat::YUV420P) == 8);
    REQUIRE(yuv_pixel_format_bit_depth(YuvPixelFormat::YUV422P) == 8);
    REQUIRE(yuv_pixel_format_bit_depth(YuvPixelFormat::YUV444P) == 8);
    REQUIRE(yuv_pixel_format_bit_depth(YuvPixelFormat::NV16) == 8);
    REQUIRE(yuv_pixel_format_bit_depth(YuvPixelFormat::YUV420P10) == 10);
    REQUIRE(yuv_pixel_format_bit_depth(YuvPixelFormat::YUV422P10) == 10);
    REQUIRE(yuv_pixel_format_bit_depth(YuvPixelFormat::YUV444P10) == 10);
    REQUIRE(yuv_pixel_format_bit_depth(YuvPixelFormat::P010) == 10);
}

// -----------------------------------------------------------------------------
// yuv_color_matrix_name and yuv_color_primaries_name.
// -----------------------------------------------------------------------------

TEST_CASE("yuv_color_matrix_name and primaries_name helpers", "[yuv]") {
    REQUIRE(std::string(yuv_color_matrix_name(YuvColorMatrix::BT601)) == "BT.601");
    REQUIRE(std::string(yuv_color_matrix_name(YuvColorMatrix::BT709)) == "BT.709");
    REQUIRE(std::string(yuv_color_matrix_name(YuvColorMatrix::BT2020_NCL)) == "BT.2020 NCL");

    REQUIRE(std::string(yuv_color_primaries_name(YuvColorPrimaries::BT601)) == "BT.601");
    REQUIRE(std::string(yuv_color_primaries_name(YuvColorPrimaries::BT709)) == "BT.709");
    REQUIRE(std::string(yuv_color_primaries_name(YuvColorPrimaries::BT2020)) == "BT.2020");
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

    SECTION("8-bit planar format keywords") {
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

    SECTION("10-bit planar format keywords") {
        YuvStreamParams p;
        guess_yuv_params_from_filename("clip_yuv420p10_1920x1080.yuv", p);
        REQUIRE(p.pixel_format == YuvPixelFormat::YUV420P10);
        REQUIRE(p.width == 1920);
        REQUIRE(p.height == 1080);

        YuvStreamParams q;
        guess_yuv_params_from_filename("clip_yuv422p10_640x480.yuv", q);
        REQUIRE(q.pixel_format == YuvPixelFormat::YUV422P10);

        YuvStreamParams r;
        guess_yuv_params_from_filename("clip_yuv444p10_640x480.yuv", r);
        REQUIRE(r.pixel_format == YuvPixelFormat::YUV444P10);
    }

    SECTION("semi-planar format keywords") {
        YuvStreamParams p;
        guess_yuv_params_from_filename("clip_p010_1920x1080.yuv", p);
        REQUIRE(p.pixel_format == YuvPixelFormat::P010);
        REQUIRE(p.width == 1920);
        REQUIRE(p.height == 1080);

        YuvStreamParams q;
        guess_yuv_params_from_filename("clip_nv16_640x480.yuv", q);
        REQUIRE(q.pixel_format == YuvPixelFormat::NV16);
        REQUIRE(q.width == 640);
        REQUIRE(q.height == 480);
    }

    SECTION("color matrix keywords") {
        YuvStreamParams p;
        guess_yuv_params_from_filename("clip_bt601_640x480.yuv", p);
        REQUIRE(p.color_matrix == YuvColorMatrix::BT601);
        REQUIRE(p.color_primaries == YuvColorPrimaries::BT601);

        YuvStreamParams q;
        guess_yuv_params_from_filename("clip_bt709_1920x1080.yuv", q);
        REQUIRE(q.color_matrix == YuvColorMatrix::BT709);
        REQUIRE(q.color_primaries == YuvColorPrimaries::BT709);

        YuvStreamParams r;
        guess_yuv_params_from_filename("clip_bt2020_3840x2160.yuv", r);
        REQUIRE(r.color_matrix == YuvColorMatrix::BT2020_NCL);
        REQUIRE(r.color_primaries == YuvColorPrimaries::BT2020);
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
// YuvRawSource tests (FFmpeg-backed, requires IDIFF_HAVE_FFMPEG).
// -----------------------------------------------------------------------------

#ifdef IDIFF_HAVE_FFMPEG

TEST_CASE("YuvRawSource decodes every 8-bit planar format", "[yuv][ffmpeg]") {
    auto fmt = GENERATE(YuvPixelFormat::YUV420P, YuvPixelFormat::YUV422P,
                        YuvPixelFormat::YUV444P);
    CAPTURE(static_cast<int>(fmt));

    YuvStreamParams p;
    p.width = 16;
    p.height = 16;
    p.pixel_format = fmt;
    std::size_t fb = 0;
    std::string path_str = write_tmp_yuv("fmt8", p, 5, fb);
    TempFile cleanup{std::filesystem::path(path_str)};

    // Scope the source so FFmpeg releases the file handle before
    // TempFile attempts deletion (required on Windows).
    {
        YuvRawSource src(path_str, p);
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
    }
}

TEST_CASE("YuvRawSource decodes 10-bit planar formats", "[yuv][ffmpeg]") {
    auto fmt = GENERATE(YuvPixelFormat::YUV420P10, YuvPixelFormat::YUV422P10,
                        YuvPixelFormat::YUV444P10);
    CAPTURE(static_cast<int>(fmt));

    YuvStreamParams p;
    p.width = 16;
    p.height = 16;
    p.pixel_format = fmt;
    std::size_t fb = 0;
    // 10-bit mid-gray = 512
    std::string path_str = write_tmp_yuv_10bit("fmt10", p, 3, 512, fb);
    TempFile cleanup{std::filesystem::path(path_str)};

    {
        YuvRawSource src(path_str, p);
        REQUIRE(src.frame_count() == 3);
        REQUIRE(src.width() == 16);
        REQUIRE(src.height() == 16);

        auto img = src.read_frame(0);
        REQUIRE(img != nullptr);
        REQUIRE(img->info().width == 16);
        REQUIRE(img->info().height == 16);
        REQUIRE(img->info().bit_depth == 8);
        REQUIRE(img->info().source_bit_depth == 10);
        REQUIRE(img->mat().channels() == 3);
    }
}

TEST_CASE("YuvRawSource decodes P010 format", "[yuv][ffmpeg]") {
    YuvStreamParams p;
    p.width = 16;
    p.height = 16;
    p.pixel_format = YuvPixelFormat::P010;
    std::size_t fb = 0;
    std::string path_str = write_tmp_p010("p010", p, 3, 512, fb);
    TempFile cleanup{std::filesystem::path(path_str)};

    {
        YuvRawSource src(path_str, p);
        REQUIRE(src.frame_count() == 3);
        REQUIRE(src.width() == 16);
        REQUIRE(src.height() == 16);

        auto img = src.read_frame(0);
        REQUIRE(img != nullptr);
        REQUIRE(img->info().width == 16);
        REQUIRE(img->info().height == 16);
        REQUIRE(img->info().source_bit_depth == 10);
    }
}

TEST_CASE("YuvRawSource decodes NV16 format", "[yuv][ffmpeg]") {
    YuvStreamParams p;
    p.width = 16;
    p.height = 16;
    p.pixel_format = YuvPixelFormat::NV16;
    std::size_t fb = 0;
    std::string path_str = write_tmp_nv16("nv16", p, 3, fb);
    TempFile cleanup{std::filesystem::path(path_str)};

    {
        YuvRawSource src(path_str, p);
        REQUIRE(src.frame_count() == 3);
        REQUIRE(src.width() == 16);
        REQUIRE(src.height() == 16);

        auto img = src.read_frame(0);
        REQUIRE(img != nullptr);
        REQUIRE(img->info().width == 16);
        REQUIRE(img->info().height == 16);
        REQUIRE(img->info().source_bit_depth == 8);
    }
}

TEST_CASE("YuvRawSource populates src_av_frame for pixel inspector",
          "[yuv][ffmpeg]") {
    YuvStreamParams p;
    p.width = 16;
    p.height = 16;
    p.pixel_format = YuvPixelFormat::YUV420P;
    std::size_t fb = 0;
    std::string path_str = write_tmp_yuv("avframe", p, 2, fb);
    TempFile cleanup{std::filesystem::path(path_str)};

    {
        YuvRawSource src(path_str, p);
        auto img = src.read_frame(0);
        REQUIRE(img != nullptr);
        // The FFmpeg path should populate src_av_frame so the pixel
        // inspector can read native YUV values.
        REQUIRE(img->internal().src_av_frame != nullptr);
    }
}

TEST_CASE("YuvRawSource: rejects invalid frame indices and invalid params",
          "[yuv][ffmpeg]") {
    SECTION("out-of-range and negative frame indices") {
        YuvStreamParams p;
        p.width = 16;
        p.height = 16;
        p.pixel_format = YuvPixelFormat::YUV420P;
        std::size_t fb = 0;
        std::string path_str = write_tmp_yuv("oob", p, 2, fb);
        TempFile cleanup{std::filesystem::path(path_str)};

        {
            YuvRawSource src(path_str, p);
            REQUIRE(src.read_frame(2) == nullptr);   // index == count
            REQUIRE(src.read_frame(-1) == nullptr);  // negative
            REQUIRE_FALSE(src.last_error().empty());
        }
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

TEST_CASE("YuvRawSource color metadata affects decode", "[yuv][ffmpeg]") {
    YuvStreamParams p;
    p.width = 16;
    p.height = 16;
    p.pixel_format = YuvPixelFormat::YUV420P;
    p.color_range = YuvColorRange::Limited;
    p.color_matrix = YuvColorMatrix::BT601;

    // Create a YUV file with a non-neutral value so the matrix
    // difference is visible.  Y=82, U=90, V=240 is an intense
    // blue that lands at different RGB values under BT.601 vs
    // BT.709 matrices.
    auto fb = yuv_frame_size_bytes(p);
    auto dir = std::filesystem::temp_directory_path();
    auto path = dir / "idiff_yuv_color.yuv";
    TempFile cleanup{path};

    std::vector<uint8_t> frame(fb);
    const int y_bytes = 16 * 16;
    const int u_bytes = 8 * 8;
    std::fill(frame.begin(), frame.begin() + y_bytes, static_cast<uint8_t>(82));
    std::fill(frame.begin() + y_bytes, frame.begin() + y_bytes + u_bytes,
              static_cast<uint8_t>(90));
    std::fill(frame.begin() + y_bytes + u_bytes, frame.end(),
              static_cast<uint8_t>(240));

    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(frame.data()), frame.size());
    }

    cv::Mat m601, m709;
    {
        YuvRawSource src_601(path.string(), p);
        auto img_601 = src_601.read_frame(0);
        REQUIRE(img_601 != nullptr);
        m601 = img_601->mat().clone();
    }

    p.color_matrix = YuvColorMatrix::BT709;
    {
        YuvRawSource src_709(path.string(), p);
        auto img_709 = src_709.read_frame(0);
        REQUIRE(img_709 != nullptr);
        m709 = img_709->mat().clone();
    }

    bool any_diff = false;
    for (int y = 0; y < m601.rows && !any_diff; y++) {
        for (int x = 0; x < m601.cols && !any_diff; x++) {
            auto p601 = m601.at<cv::Vec3b>(y, x);
            auto p709 = m709.at<cv::Vec3b>(y, x);
            if (p601 != p709) any_diff = true;
        }
    }
    REQUIRE(any_diff);
}

#endif // IDIFF_HAVE_FFMPEG
