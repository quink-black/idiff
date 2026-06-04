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

// Build a synthetic planar 8-bit YUV frame and write `frame_count`
// copies to a temp file.  Returns the path.
std::string write_tmp_yuv(const std::string& tag, int width, int height,
                          int frame_count,
                          const std::string& pixel_format,
                          std::size_t frame_bytes) {
    auto dir = std::filesystem::temp_directory_path();
    auto path = dir / (std::string("idiff_yuv_") + tag + ".yuv");

    std::vector<uint8_t> frame(frame_bytes);
    std::fill(frame.begin(), frame.end(), static_cast<uint8_t>(128));

    std::ofstream out(path, std::ios::binary);
    REQUIRE(out.is_open());
    for (int i = 0; i < frame_count; i++) {
        // First byte of each frame = frame index mod 256.
        frame[0] = static_cast<uint8_t>(i & 0xFF);
        out.write(reinterpret_cast<const char*>(frame.data()), frame.size());
    }
    out.close();
    return path.string();
}

// Build a synthetic 10-bit planar YUV frame (16-bit LE samples) and
// write `frame_count` copies to a temp file.
std::string write_tmp_yuv_10bit(const std::string& tag, int width, int height,
                                int frame_count, uint16_t value10,
                                const std::string& pixel_format,
                                std::size_t frame_bytes) {
    auto dir = std::filesystem::temp_directory_path();
    auto path = dir / (std::string("idiff_yuv10_") + tag + ".yuv");

    // value10 in the lower 10 bits, stored as 16-bit LE.
    uint16_t val = value10 & 0x3FF;
    uint8_t lo = static_cast<uint8_t>(val & 0xFF);
    uint8_t hi = static_cast<uint8_t>((val >> 8) & 0xFF);

    std::vector<uint8_t> frame(frame_bytes);
    for (std::size_t i = 0; i + 1 < frame_bytes; i += 2) {
        frame[i] = lo;
        frame[i + 1] = hi;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.is_open());
    for (int i = 0; i < frame_count; i++) {
        frame[0] = static_cast<uint8_t>(i & 0xFF);
        out.write(reinterpret_cast<const char*>(frame.data()), frame.size());
    }
    out.close();
    return path.string();
}

// Compute frame size for common formats (used by test helpers only;
// production code uses FFmpeg's av_image_get_buffer_size).
std::size_t test_frame_size(const std::string& fmt, int w, int h) {
    // 8-bit planar
    if (fmt == "yuv420p") return static_cast<std::size_t>(w * h * 3 / 2);
    if (fmt == "yuv422p") return static_cast<std::size_t>(w * h * 2);
    if (fmt == "yuv444p") return static_cast<std::size_t>(w * h * 3);
    if (fmt == "nv12")    return static_cast<std::size_t>(w * h + (w / 2) * (h / 2) * 2);
    // 10-bit planar (16-bit LE per sample)
    if (fmt == "yuv420p10le") return static_cast<std::size_t>(w * h * 2 + (w / 2) * (h / 2) * 2 * 2);
    if (fmt == "yuv422p10le") return static_cast<std::size_t>(w * h * 2 + (w / 2) * h * 2 * 2);
    if (fmt == "yuv444p10le") return static_cast<std::size_t>(w * h * 2 * 3);
    if (fmt == "p010le")     return static_cast<std::size_t>(w * h * 2 + (w / 2) * (h / 2) * 2 * 2);
    return 0;
}

// Helper to make a YuvStreamParams.
YuvStreamParams make_params(int w, int h, const std::string& fmt) {
    YuvStreamParams p;
    p.width = w;
    p.height = h;
    p.pixel_format = fmt;
    return p;
}

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

}  // namespace

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
        REQUIRE(p.pixel_format == "yuv422p");
        REQUIRE(p.width == 640);
        REQUIRE(p.height == 480);

        YuvStreamParams q;
        guess_yuv_params_from_filename("clip_yuv444p_640x480.yuv", q);
        REQUIRE(q.pixel_format == "yuv444p");

        YuvStreamParams r;
        guess_yuv_params_from_filename("something_i420.yuv", r);
        REQUIRE(r.pixel_format == "yuv420p");
    }

    SECTION("10-bit planar format keywords") {
        YuvStreamParams p;
        guess_yuv_params_from_filename("clip_yuv420p10_1920x1080.yuv", p);
        REQUIRE(p.pixel_format == "yuv420p10le");
        REQUIRE(p.width == 1920);
        REQUIRE(p.height == 1080);

        YuvStreamParams q;
        guess_yuv_params_from_filename("clip_yuv422p10_640x480.yuv", q);
        REQUIRE(q.pixel_format == "yuv422p10le");

        YuvStreamParams r;
        guess_yuv_params_from_filename("clip_yuv444p10_640x480.yuv", r);
        REQUIRE(r.pixel_format == "yuv444p10le");
    }

    SECTION("semi-planar format keywords") {
        YuvStreamParams p;
        guess_yuv_params_from_filename("clip_p010_1920x1080.yuv", p);
        REQUIRE(p.pixel_format == "p010le");
        REQUIRE(p.width == 1920);
        REQUIRE(p.height == 1080);

        YuvStreamParams q;
        guess_yuv_params_from_filename("clip_nv12_640x480.yuv", q);
        REQUIRE(q.pixel_format == "nv12");
        REQUIRE(q.width == 640);
        REQUIRE(q.height == 480);
    }

    SECTION("color metadata keywords use FFmpeg names") {
        YuvStreamParams p;
        guess_yuv_params_from_filename("clip_bt601_640x480.yuv", p);
        REQUIRE(p.color_matrix == "smpte170m");
        REQUIRE(p.color_primaries == "smpte170m");

        YuvStreamParams q;
        guess_yuv_params_from_filename("clip_bt709_1920x1080.yuv", q);
        REQUIRE(q.color_matrix == "bt709");
        REQUIRE(q.color_primaries == "bt709");

        YuvStreamParams r;
        guess_yuv_params_from_filename("clip_bt2020_3840x2160.yuv", r);
        REQUIRE(r.color_matrix == "bt2020-nccl");
        REQUIRE(r.color_primaries == "bt2020");
    }

    SECTION("transfer function keywords use FFmpeg names") {
        YuvStreamParams p;
        guess_yuv_params_from_filename("clip_p010_3840x2160_pq.yuv", p);
        REQUIRE(p.transfer == "smpte2084");

        YuvStreamParams q;
        guess_yuv_params_from_filename("clip_p010_3840x2160_hlg.yuv", q);
        REQUIRE(q.transfer == "arib-std-b67");

        YuvStreamParams r;
        guess_yuv_params_from_filename("clip_smpte2084_3840x2160.yuv", r);
        REQUIRE(r.transfer == "smpte2084");
    }

    SECTION("color range keywords use FFmpeg names") {
        YuvStreamParams p;
        guess_yuv_params_from_filename("clip_fullrange_640x480.yuv", p);
        REQUIRE(p.color_range == "pc");

        YuvStreamParams q;
        guess_yuv_params_from_filename("clip_limited_640x480.yuv", q);
        REQUIRE(q.color_range == "tv");
    }

    SECTION("unrecognized filename leaves the params untouched") {
        YuvStreamParams p;
        p.width = 0;
        p.height = 0;
        REQUIRE_FALSE(guess_yuv_params_from_filename("plain.yuv", p));
        REQUIRE(p.width == 0);
        REQUIRE(p.height == 0);
    }

    SECTION("defaults are sensible FFmpeg names") {
        YuvStreamParams p;
        REQUIRE(p.pixel_format == "yuv420p");
        REQUIRE(p.color_range == "tv");
        REQUIRE(p.color_matrix == "bt709");
        REQUIRE(p.color_primaries == "bt709");
        REQUIRE(p.transfer == "bt709");
    }
}

// -----------------------------------------------------------------------------
// YuvRawSource tests (FFmpeg-backed, requires IDIFF_HAVE_FFMPEG).
// -----------------------------------------------------------------------------

#ifdef IDIFF_HAVE_FFMPEG

TEST_CASE("YuvRawSource decodes every 8-bit planar format", "[yuv][ffmpeg]") {
    auto fmt = GENERATE("yuv420p", "yuv422p", "yuv444p");
    CAPTURE(fmt);

    auto p = make_params(16, 16, fmt);
    auto fb = test_frame_size(fmt, 16, 16);
    REQUIRE(fb > 0);
    std::string path_str = write_tmp_yuv("fmt8", 16, 16, 5, fmt, fb);
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
    auto fmt = GENERATE("yuv420p10le", "yuv422p10le", "yuv444p10le");
    CAPTURE(fmt);

    auto p = make_params(16, 16, fmt);
    auto fb = test_frame_size(fmt, 16, 16);
    REQUIRE(fb > 0);
    std::string path_str = write_tmp_yuv_10bit("fmt10", 16, 16, 3, 512, fmt, fb);
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
    auto p = make_params(16, 16, "p010le");
    auto fb = test_frame_size("p010le", 16, 16);
    REQUIRE(fb > 0);
    std::string path_str = write_tmp_yuv_10bit("p010", 16, 16, 3, 512, "p010le", fb);
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

TEST_CASE("YuvRawSource decodes NV12 format", "[yuv][ffmpeg]") {
    auto p = make_params(16, 16, "nv12");
    auto fb = test_frame_size("nv12", 16, 16);
    REQUIRE(fb > 0);
    std::string path_str = write_tmp_yuv("nv12", 16, 16, 3, "nv12", fb);
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

TEST_CASE("YuvRawSource decodes arbitrary FFmpeg format name",
          "[yuv][ffmpeg]") {
    auto p = make_params(16, 16, "yuv444p");
    auto fb = test_frame_size("yuv444p", 16, 16);
    std::string path_str = write_tmp_yuv("arb", 16, 16, 2, "yuv444p", fb);
    TempFile cleanup{std::filesystem::path(path_str)};

    {
        YuvRawSource src(path_str, p);
        REQUIRE(src.frame_count() == 2);
        auto img = src.read_frame(0);
        REQUIRE(img != nullptr);
        REQUIRE(img->info().source_bit_depth == 8);
    }
}

TEST_CASE("YuvRawSource populates src_av_frame for pixel inspector",
          "[yuv][ffmpeg]") {
    auto p = make_params(16, 16, "yuv420p");
    auto fb = test_frame_size("yuv420p", 16, 16);
    std::string path_str = write_tmp_yuv("avframe", 16, 16, 2, "yuv420p", fb);
    TempFile cleanup{std::filesystem::path(path_str)};

    {
        YuvRawSource src(path_str, p);
        auto img = src.read_frame(0);
        REQUIRE(img != nullptr);
        REQUIRE(img->internal().src_av_frame != nullptr);
    }
}

TEST_CASE("YuvRawSource: rejects invalid frame indices and invalid params",
          "[yuv][ffmpeg]") {
    SECTION("out-of-range and negative frame indices") {
        auto p = make_params(16, 16, "yuv420p");
        auto fb = test_frame_size("yuv420p", 16, 16);
        std::string path_str = write_tmp_yuv("oob", 16, 16, 2, "yuv420p", fb);
        TempFile cleanup{std::filesystem::path(path_str)};

        {
            YuvRawSource src(path_str, p);
            REQUIRE(src.read_frame(2) == nullptr);
            REQUIRE(src.read_frame(-1) == nullptr);
            REQUIRE_FALSE(src.last_error().empty());
        }
    }

    SECTION("invalid pixel format surfaces as open failure") {
        auto p = make_params(16, 16, "nonexistent_pixfmt_xyz");
        YuvRawSource src("/tmp/does_not_matter.yuv", p);
        REQUIRE(src.read_frame(0) == nullptr);
        REQUIRE_FALSE(src.last_error().empty());
    }
}

TEST_CASE("YuvRawSource color metadata affects decode", "[yuv][ffmpeg]") {
    YuvStreamParams p;
    p.width = 16;
    p.height = 16;
    p.pixel_format = "yuv420p";
    p.color_range = "tv";
    p.color_matrix = "smpte170m";

    auto fb = test_frame_size("yuv420p", 16, 16);
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

    p.color_matrix = "bt709";
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

TEST_CASE("YuvRawSource rejects unknown pixel format name",
          "[yuv][ffmpeg]") {
    auto p = make_params(16, 16, "not_a_real_format");
    auto fb = test_frame_size("yuv420p", 16, 16);
    std::string path_str = write_tmp_yuv("badfmt", 16, 16, 1, "yuv420p", fb);
    TempFile cleanup{std::filesystem::path(path_str)};

    YuvRawSource src(path_str, p);
    auto img = src.read_frame(0);
    REQUIRE(img == nullptr);
    REQUIRE_FALSE(src.last_error().empty());
}

#endif // IDIFF_HAVE_FFMPEG
