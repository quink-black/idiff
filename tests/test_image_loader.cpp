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
    // NOTE: we use \uXXXX escapes so the source file stays plain ASCII;
    // MSVC is configured with /utf-8 so the string literal is encoded
    // in UTF-8 at compile time, which is what ImageLoader expects.
    const std::string leaf =
        std::string("idiff_utf8_") + tag + "_=\xe4\xb8\x8b\xe8\xbd\xbd\xe4\xb8\xad=";
    auto dir = std::filesystem::temp_directory_path() /
               std::filesystem::u8path(leaf);
    std::filesystem::create_directories(dir);
    return dir;
}

} // namespace

TEST_CASE("ImageLoader: nonexistent file returns nullptr", "[image_loader]")
{
    ImageLoader loader;
    auto img = loader.load("/nonexistent/path/image.png");
    REQUIRE(img == nullptr);
    REQUIRE_FALSE(loader.last_error().empty());
}

TEST_CASE("ImageLoader: empty memory returns nullptr", "[image_loader]")
{
    ImageLoader loader;
    auto img = loader.load_from_memory(nullptr, 0);
    REQUIRE(img == nullptr);
}

TEST_CASE("ImageLoader: RAW extension detection", "[image_loader]")
{
    REQUIRE(RawLoader::is_raw_extension("photo.dng"));
    REQUIRE(RawLoader::is_raw_extension("photo.CR2"));
    REQUIRE(RawLoader::is_raw_extension("photo.NEF"));
    REQUIRE_FALSE(RawLoader::is_raw_extension("photo.png"));
    REQUIRE_FALSE(RawLoader::is_raw_extension("photo.jpg"));
}

TEST_CASE("platform::read_file_binary round-trips bytes through a non-ASCII path",
          "[platform_utf8]")
{
    auto dir = make_non_ascii_tmp_dir("rw");
    auto file = dir / std::filesystem::u8path(
        "a_\xe4\xb8\xad.bin"); // "a_<中>.bin"

    const std::vector<uint8_t> payload = {0x00, 0xff, 0x42, 0x13, 0x37};
    REQUIRE(platform::write_file_binary(file.u8string(),
                                        payload.data(), payload.size()));

    auto got = platform::read_file_binary(file.u8string());
    REQUIRE(got == payload);

    std::filesystem::remove_all(dir);
}

TEST_CASE("platform::read_file_binary on missing file returns empty, not crash",
          "[platform_utf8]")
{
    auto dir = make_non_ascii_tmp_dir("missing");
    auto file = dir / std::filesystem::u8path("does_not_exist_\xe4\xb8\xad.jpg");

    auto got = platform::read_file_binary(file.u8string());
    REQUIRE(got.empty());

    std::filesystem::remove_all(dir);
}

TEST_CASE("platform::read_file_binary tolerates a zero-byte file",
          "[platform_utf8]")
{
    auto dir = make_non_ascii_tmp_dir("empty");
    auto file = dir / std::filesystem::u8path("empty_\xe4\xb8\xad.bin");

    // Create a zero-byte file via the helper itself.
    REQUIRE(platform::write_file_binary(file.u8string(), nullptr, 0));

    // Zero-byte files are reported as "empty buffer" by read_file_binary;
    // that's semantically indistinguishable from "file missing" in this
    // helper, but the call must not crash on the nullptr data pointer
    // a zero-sized std::vector hands back.
    auto got = platform::read_file_binary(file.u8string());
    REQUIRE(got.empty());

    std::filesystem::remove_all(dir);
}

TEST_CASE("ImageLoader::load handles a non-ASCII path", "[image_loader][utf8]")
{
    auto dir = make_non_ascii_tmp_dir("load");
    auto file = dir / std::filesystem::u8path("a.jpg");

    // Create a valid JPEG via cv::imencode + the UTF-8-safe writer, so
    // we exercise the same write path the app uses for Save Viewport.
    cv::Mat m(8, 8, CV_8UC3, cv::Scalar(40, 80, 160));
    std::vector<uint8_t> buf;
    REQUIRE(cv::imencode(".jpg", m, buf));
    REQUIRE(platform::write_file_binary(file.u8string(), buf.data(), buf.size()));

    ImageLoader loader;
    auto img = loader.load(file.u8string());
    REQUIRE(img != nullptr);
    REQUIRE(img->info().width == 8);
    REQUIRE(img->info().height == 8);

    std::filesystem::remove_all(dir);
}

TEST_CASE("ImageLoader::load on a non-ASCII path pointing at nothing fails cleanly",
          "[image_loader][utf8]")
{
    auto dir = make_non_ascii_tmp_dir("missing_load");
    auto file = dir / std::filesystem::u8path("missing.jpg");

    ImageLoader loader;
    auto img = loader.load(file.u8string());
    REQUIRE(img == nullptr);
    REQUIRE_FALSE(loader.last_error().empty());

    std::filesystem::remove_all(dir);
}

// Exercise the exact path shapes that users reported crashing on:
// leading '=', trailing '=', mixed ASCII and CJK.  We create each
// directory under the system temp dir, drop a tiny JPEG in it, and
// then load it through ImageLoader.  The point of the test is that
// none of these should crash -- they must either load successfully
// or fail gracefully.
TEST_CASE("ImageLoader::load survives the reported crashing path shapes",
          "[image_loader][utf8][crash]")
{
    // UTF-8 encodings of '下载中' ("downloading") and '中' ("middle").
    const char* cjk_xiazaizhong = "\xe4\xb8\x8b\xe8\xbd\xbd\xe4\xb8\xad";
    const char* cjk_zhong       = "\xe4\xb8\xad";

    const std::vector<std::string> leaves = {
        std::string("=") + cjk_xiazaizhong + "=",  // "=下载中="
        std::string(cjk_xiazaizhong) + "=",        //  "下载中="
        std::string(cjk_zhong) + "=",              //  "中="
    };

    for (const auto& leaf : leaves) {
        auto dir = std::filesystem::temp_directory_path() /
                   std::filesystem::u8path("idiff_crash_" + leaf);
        std::filesystem::create_directories(dir);
        auto file = dir / std::filesystem::u8path("a.jpg");

        cv::Mat m(8, 8, CV_8UC3, cv::Scalar(1, 2, 3));
        std::vector<uint8_t> buf;
        REQUIRE(cv::imencode(".jpg", m, buf));
        REQUIRE(platform::write_file_binary(file.u8string(),
                                            buf.data(), buf.size()));

        ImageLoader loader;
        auto img = loader.load(file.u8string());
        INFO("path: " << file.u8string());
        REQUIRE(img != nullptr);
        REQUIRE(img->info().width  == 8);
        REQUIRE(img->info().height == 8);

        std::filesystem::remove_all(dir);
    }
}

// The Save Viewport code path in app.cpp infers the image format from
// the filename extension and then routes the bytes through
// cv::imencode + platform::write_file_binary.  Make sure that write
// side also survives non-ASCII paths and that a subsequent load picks
// up exactly the pixels we wrote.
TEST_CASE("imencode + platform::write_file_binary round-trips a non-ASCII path",
          "[image_loader][utf8]")
{
    auto dir = make_non_ascii_tmp_dir("save");
    auto file = dir / std::filesystem::u8path("saved_\xe4\xb8\xad.png");

    cv::Mat expected(4, 6, CV_8UC3);
    for (int y = 0; y < expected.rows; ++y)
        for (int x = 0; x < expected.cols; ++x)
            expected.at<cv::Vec3b>(y, x) = cv::Vec3b(x * 10, y * 20, 50);

    std::vector<uint8_t> buf;
    REQUIRE(cv::imencode(".png", expected, buf));
    REQUIRE(platform::write_file_binary(file.u8string(), buf.data(), buf.size()));

    ImageLoader loader;
    auto img = loader.load(file.u8string());
    REQUIRE(img != nullptr);
    REQUIRE(img->info().width  == 6);
    REQUIRE(img->info().height == 4);

    std::filesystem::remove_all(dir);
}

// Paths that cannot be interpreted as UTF-8 (stray high bytes that
// don't form a valid sequence) must not crash the loader.  They just
// need to report failure.  This catches the case where the Windows
// UTF-8 to UTF-16 conversion fails and downstream code dereferences
// the resulting empty / partial wide string.
TEST_CASE("ImageLoader::load handles invalid UTF-8 paths without crashing",
          "[image_loader][utf8]")
{
    // 0xFF is never valid in a UTF-8 byte sequence.
    const std::string bad_path = "D:/\xff\xff_bad/image.png";

    ImageLoader loader;
    auto img = loader.load(bad_path);
    REQUIRE(img == nullptr);
    REQUIRE_FALSE(loader.last_error().empty());
}

#ifdef _WIN32
// Windows-specific: real users reported crashes when loading files
// through the native backslash-separated drive path.  Create a file
// at such a path and make sure it loads through ImageLoader.  This
// is in addition to the forward-slash tests above because the
// platform code goes through CreateFileW, which accepts either
// separator, but the rest of the pipeline (extension sniffing,
// last_error messages) includes the raw path string.
TEST_CASE("ImageLoader::load accepts backslash Windows paths with CJK",
          "[image_loader][utf8][windows]")
{
    auto dir = make_non_ascii_tmp_dir("winslash");
    // Build a backslash-separated version of the path string, which
    // is what NFD and drag-and-drop deliver on Windows.
    std::string forward = (dir / std::filesystem::u8path("a.jpg")).u8string();
    std::string backslash = forward;
    for (auto& c : backslash) if (c == '/') c = '\\';

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