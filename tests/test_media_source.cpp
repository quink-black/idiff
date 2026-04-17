#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <filesystem>
#include <string>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include "core/media_source.h"
#include "core/image.h"

using namespace idiff;

namespace {

// Write a tiny RGB PNG to a unique temp path and return the path.  The
// caller is responsible for removing the file.
std::string write_tmp_png(const std::string& tag) {
    auto dir = std::filesystem::temp_directory_path();
    auto path = dir / (std::string("idiff_media_source_") + tag + ".png");
    cv::Mat m = cv::Mat::zeros(4, 6, CV_8UC3);
    // Put a non-trivial pattern so decoding actually has to do something.
    m.at<cv::Vec3b>(1, 2) = cv::Vec3b(10, 20, 30);
    REQUIRE(cv::imwrite(path.string(), m));
    return path.string();
}

} // namespace

TEST_CASE("ImageFileSource: nonexistent file fails read_frame", "[media_source]")
{
    ImageFileSource src("/nonexistent/path/nope.png",
                        ImageLoader::default_backend());
    REQUIRE(src.frame_count() == 1);
    auto img = src.read_frame(0);
    REQUIRE(img == nullptr);
    REQUIRE_FALSE(src.last_error().empty());
}

TEST_CASE("ImageFileSource: read_frame(0) decodes a real file", "[media_source]")
{
    std::string path = write_tmp_png("decode");
    ImageFileSource src(path, ImageLoader::default_backend());

    auto img = src.read_frame(0);
    REQUIRE(img != nullptr);
    REQUIRE(img->info().width == 6);
    REQUIRE(img->info().height == 4);

    // Dimensions and format description should have been populated.
    REQUIRE(src.width() == 6);
    REQUIRE(src.height() == 4);
    REQUIRE_FALSE(src.format_description().empty());
    REQUIRE(src.last_error().empty());

    std::filesystem::remove(path);
}

TEST_CASE("ImageFileSource: out-of-range frame index is rejected", "[media_source]")
{
    std::string path = write_tmp_png("oob");
    ImageFileSource src(path, ImageLoader::default_backend());

    auto img = src.read_frame(1);
    REQUIRE(img == nullptr);
    REQUIRE_FALSE(src.last_error().empty());

    // Negative indices too.
    auto img2 = src.read_frame(-1);
    REQUIRE(img2 == nullptr);

    std::filesystem::remove(path);
}

TEST_CASE("ImageFileSource: always reports exactly one frame", "[media_source]")
{
    ImageFileSource src("/some/path.png", ImageLoader::default_backend());
    REQUIRE(src.frame_count() == 1);
}
