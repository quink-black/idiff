#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include "core/image_loader.h"
#include "core/detail/raw_loader.h"
#include "core/detail/platform_utf8.h"

using namespace idiff;

namespace {

// A unique temp dir whose leaf name contains non-ASCII characters.
// `tag` lets multiple tests coexist without racing on the same dir.
std::filesystem::path make_non_ascii_tmp_dir(const std::string& tag) {
    // Mix ASCII and CJK to stress both the UTF-8→UTF-16 conversion and
    // the surrounding path segments.  The '=' prefix/suffix reproduces
    // the exact shape reported by the crashing user.
    //
    // NOTE: we use \xXX escapes so the source file stays plain ASCII;
    // MSVC is configured with /utf-8 so the string literal is encoded
    // in UTF-8 at compile time, which is what ImageLoader expects.
    const std::string leaf =
        std::string("idiff_utf8_") + tag +
        "_=\xe4\xb8\x8b\xe8\xbd\xbd\xe4\xb8\xad=";
    auto dir = std::filesystem::temp_directory_path() /
               std::filesystem::u8path(leaf);
    std::filesystem::create_directories(dir);
    return dir;
}

}  // namespace

// -----------------------------------------------------------------------------
// Trivial entry-point sanity checks.
// -----------------------------------------------------------------------------

TEST_CASE("ImageLoader: rejects clearly invalid inputs", "[image_loader]") {
    ImageLoader loader;

    SECTION("nonexistent file path") {
        auto img = loader.load("/nonexistent/path/image.png");
        REQUIRE(img == nullptr);
        REQUIRE_FALSE(loader.last_error().empty());
    }
    SECTION("empty memory buffer") {
        REQUIRE(loader.load_from_memory(nullptr, 0) == nullptr);
    }
}

TEST_CASE("ImageLoader: RAW extension detection", "[image_loader]") {
    REQUIRE(RawLoader::is_raw_extension("photo.dng"));
    REQUIRE(RawLoader::is_raw_extension("photo.CR2"));
    REQUIRE(RawLoader::is_raw_extension("photo.NEF"));
    REQUIRE_FALSE(RawLoader::is_raw_extension("photo.png"));
    REQUIRE_FALSE(RawLoader::is_raw_extension("photo.jpg"));
}

// -----------------------------------------------------------------------------
// platform::read_file_binary / write_file_binary.
// -----------------------------------------------------------------------------

TEST_CASE("platform binary I/O survives non-ASCII paths and edge cases",
          "[platform_utf8]") {
    SECTION("write then read round-trips bytes") {
        auto dir = make_non_ascii_tmp_dir("rw");
        auto file =
            dir / std::filesystem::u8path("a_\xe4\xb8\xad.bin");  // a_<中>.bin

        const std::vector<uint8_t> payload = {0x00, 0xff, 0x42, 0x13, 0x37};
        REQUIRE(platform::write_file_binary(file.u8string(), payload.data(),
                                            payload.size()));
        REQUIRE(platform::read_file_binary(file.u8string()) == payload);

        std::filesystem::remove_all(dir);
    }
    SECTION("read of a missing file returns empty without crashing") {
        auto dir = make_non_ascii_tmp_dir("missing");
        auto file = dir / std::filesystem::u8path(
                              "does_not_exist_\xe4\xb8\xad.jpg");

        REQUIRE(platform::read_file_binary(file.u8string()).empty());
        std::filesystem::remove_all(dir);
    }
    SECTION("read of a zero-byte file does not crash on the null pointer") {
        // Helper accepts data=nullptr when size==0.
        auto dir = make_non_ascii_tmp_dir("empty");
        auto file = dir / std::filesystem::u8path("empty_\xe4\xb8\xad.bin");
        REQUIRE(platform::write_file_binary(file.u8string(), nullptr, 0));
        REQUIRE(platform::read_file_binary(file.u8string()).empty());
        std::filesystem::remove_all(dir);
    }
}

// -----------------------------------------------------------------------------
// ImageLoader against non-ASCII / pathological path shapes.
// -----------------------------------------------------------------------------

TEST_CASE("ImageLoader: non-ASCII paths load, fail and round-trip safely",
          "[image_loader][utf8]") {
    SECTION("loads a JPEG encoded through the UTF-8 writer") {
        auto dir = make_non_ascii_tmp_dir("load");
        auto file = dir / std::filesystem::u8path("a.jpg");

        cv::Mat m(8, 8, CV_8UC3, cv::Scalar(40, 80, 160));
        std::vector<uint8_t> buf;
        REQUIRE(cv::imencode(".jpg", m, buf));
        REQUIRE(platform::write_file_binary(file.u8string(), buf.data(),
                                            buf.size()));

        ImageLoader loader;
        auto img = loader.load(file.u8string());
        REQUIRE(img != nullptr);
        REQUIRE(img->info().width == 8);
        REQUIRE(img->info().height == 8);
        std::filesystem::remove_all(dir);
    }

    SECTION("missing file with non-ASCII path fails cleanly") {
        auto dir = make_non_ascii_tmp_dir("missing_load");
        auto file = dir / std::filesystem::u8path("missing.jpg");

        ImageLoader loader;
        REQUIRE(loader.load(file.u8string()) == nullptr);
        REQUIRE_FALSE(loader.last_error().empty());
        std::filesystem::remove_all(dir);
    }

    SECTION("survives the reported crashing path shapes") {
        // UTF-8 encodings of '下载中' and '中'.
        const char* xiazaizhong = "\xe4\xb8\x8b\xe8\xbd\xbd\xe4\xb8\xad";
        const char* zhong = "\xe4\xb8\xad";
        const std::vector<std::string> leaves = {
            std::string("=") + xiazaizhong + "=",  // "=下载中="
            std::string(xiazaizhong) + "=",        //  "下载中="
            std::string(zhong) + "=",              //  "中="
        };
        for (const auto& leaf : leaves) {
            auto dir = std::filesystem::temp_directory_path() /
                       std::filesystem::u8path("idiff_crash_" + leaf);
            std::filesystem::create_directories(dir);
            auto file = dir / std::filesystem::u8path("a.jpg");

            cv::Mat m(8, 8, CV_8UC3, cv::Scalar(1, 2, 3));
            std::vector<uint8_t> buf;
            REQUIRE(cv::imencode(".jpg", m, buf));
            REQUIRE(platform::write_file_binary(file.u8string(), buf.data(),
                                                buf.size()));

            ImageLoader loader;
            auto img = loader.load(file.u8string());
            INFO("path: " << file.u8string());
            REQUIRE(img != nullptr);
            REQUIRE(img->info().width == 8);
            REQUIRE(img->info().height == 8);
            std::filesystem::remove_all(dir);
        }
    }

    SECTION("imencode + write_file_binary round-trips a PNG") {
        // Mirrors the Save Viewport code path in app.cpp.
        auto dir = make_non_ascii_tmp_dir("save");
        auto file =
            dir / std::filesystem::u8path("saved_\xe4\xb8\xad.png");

        cv::Mat expected(4, 6, CV_8UC3);
        for (int y = 0; y < expected.rows; ++y)
            for (int x = 0; x < expected.cols; ++x)
                expected.at<cv::Vec3b>(y, x) = cv::Vec3b(x * 10, y * 20, 50);

        std::vector<uint8_t> buf;
        REQUIRE(cv::imencode(".png", expected, buf));
        REQUIRE(platform::write_file_binary(file.u8string(), buf.data(),
                                            buf.size()));

        ImageLoader loader;
        auto img = loader.load(file.u8string());
        REQUIRE(img != nullptr);
        REQUIRE(img->info().width == 6);
        REQUIRE(img->info().height == 4);
        std::filesystem::remove_all(dir);
    }

    SECTION("invalid UTF-8 path fails without crashing") {
        // 0xFF is never valid in a UTF-8 byte sequence.
        const std::string bad_path = "D:/\xff\xff_bad/image.png";
        ImageLoader loader;
        REQUIRE(loader.load(bad_path) == nullptr);
        REQUIRE_FALSE(loader.last_error().empty());
    }
}

#ifdef _WIN32
// Windows-specific: real users reported crashes when loading files
// through the native backslash-separated drive path.  CreateFileW
// accepts either separator, but the rest of the pipeline (extension
// sniffing, last_error messages) includes the raw path string.
TEST_CASE("ImageLoader::load accepts backslash Windows paths with CJK",
          "[image_loader][utf8][windows]") {
    auto dir = make_non_ascii_tmp_dir("winslash");
    std::string forward =
        (dir / std::filesystem::u8path("a.jpg")).u8string();
    std::string backslash = forward;
    for (auto& c : backslash)
        if (c == '/') c = '\\';

    cv::Mat m(8, 8, CV_8UC3, cv::Scalar(7, 8, 9));
    std::vector<uint8_t> buf;
    REQUIRE(cv::imencode(".jpg", m, buf));
    REQUIRE(platform::write_file_binary(backslash, buf.data(), buf.size()));

    ImageLoader loader;
    auto img = loader.load(backslash);
    REQUIRE(img != nullptr);
    REQUIRE(img->info().width == 8);

    std::filesystem::remove_all(dir);
}
#endif

// -----------------------------------------------------------------------------
// HEIF / AVIF FFmpeg backend
// -----------------------------------------------------------------------------
//
// These cases need real sample files; without them they SKIP rather
// than fail.  IDIFF_TEST_HEIF_TILE_PATH is set by tests/CMakeLists.txt
// to either tests/data/heif-tile.heif or the developer's
// ~/Downloads/heif-tile.heif when present.

#if defined(IDIFF_HAVE_FFMPEG_IMAGE_DECODE)

namespace {

bool sample_exists(const char* path) {
    return path && *path != '\0' &&
           std::filesystem::exists(std::filesystem::u8path(path));
}

}  // namespace

TEST_CASE("ImageLoader: FFmpeg backend is reported as compiled in",
          "[image_loader][heif]") {
    REQUIRE(ImageLoader::has_backend(LoaderBackend::FFmpeg));
    REQUIRE(std::string(ImageLoader::backend_name(LoaderBackend::FFmpeg))
            == "FFmpeg");
}

TEST_CASE("ImageLoader: opens a multi-tile HEIF grid as a single full image",
          "[image_loader][heif][grid]") {
    if (!sample_exists(IDIFF_TEST_HEIF_TILE_PATH)) {
        SKIP("HEIF tile sample not available "
             "(IDIFF_TEST_HEIF_TILE_PATH is empty)");
    }

    ImageLoader loader;
    auto img = loader.load(IDIFF_TEST_HEIF_TILE_PATH);
    REQUIRE(img != nullptr);
    REQUIRE(loader.last_used_backend() == LoaderBackend::FFmpeg);

    const auto& info = img->info();

    // HEIF grids -- like the sample at hand -- are usually composed
    // from many 512x512 tiles.  The whole point of tile-grid support
    // is that the loader returns ONE composed image, not one tile, so
    // the reported dimensions must be strictly larger than a single
    // tile.  We don't pin exact W/H because the sample may change.
    REQUIRE(info.width  > 512);
    REQUIRE(info.height > 512);

    // Pixel data must be present and match the reported shape.
    REQUIRE(img->mat().cols == info.width);
    REQUIRE(img->mat().rows == info.height);

    REQUIRE(info.source_format == SourceFormat::HEIF);
}

#endif  // IDIFF_HAVE_FFMPEG_IMAGE_DECODE
