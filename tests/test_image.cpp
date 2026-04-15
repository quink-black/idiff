#include <catch2/catch_test_macros.hpp>

#include "core/image.h"
#include "core/image_impl.h"

using namespace idiff;

TEST_CASE("Image: default construct has empty info", "[image]")
{
    Image img;
    const auto& info = img.info();
    REQUIRE(info.width == 0);
    REQUIRE(info.height == 0);
    REQUIRE(info.bit_depth == 8);
    REQUIRE(info.has_alpha == false);
}

TEST_CASE("Image: move construction transfers data", "[image]")
{
    auto img = std::make_unique<Image>();
    img->internal().info.width = 640;
    img->internal().info.height = 480;
    img->internal().info.source_format = SourceFormat::PNG;

    Image moved_to = std::move(*img);
    REQUIRE(moved_to.info().width == 640);
    REQUIRE(moved_to.info().height == 480);
    REQUIRE(moved_to.info().source_format == SourceFormat::PNG);

    // Moved-from object must not crash on info() access
    const auto& info = img->info();
    REQUIRE(info.width == 0);
    REQUIRE(info.height == 0);
}

TEST_CASE("Image: move assignment transfers data", "[image]")
{
    auto img = std::make_unique<Image>();
    img->internal().info.width = 1920;
    img->internal().info.height = 1080;

    Image target;
    target = std::move(*img);

    REQUIRE(target.info().width == 1920);
    REQUIRE(target.info().height == 1080);

    // Moved-from object must not crash
    const auto& info = img->info();
    REQUIRE(info.width == 0);
}

TEST_CASE("Image: moved-from unique_ptr dereference is safe", "[image]")
{
    // This is the exact crash scenario: entry.image is a unique_ptr<Image>
    // that had std::move(*entry.image) applied, leaving entry.image pointing
    // to a moved-from Image with null impl_.
    auto img = std::make_unique<Image>();
    img->internal().info.width = 100;
    img->internal().info.height = 200;

    // Simulate what update_display_image did: move from the dereferenced Image
    Image display = std::move(*img);

    // display got the data
    REQUIRE(display.info().width == 100);
    REQUIRE(display.info().height == 200);

    // img still points to an object, but it's moved-from.
    // Calling info() on it must not crash (was SIGSEGV before fix).
    const auto& info = img->info();
    REQUIRE(info.width == 0);
    REQUIRE(info.height == 0);

    // pixels() must also not crash on moved-from
    REQUIRE(img->pixels() == nullptr);

    // mat() must not crash
    REQUIRE(img->mat().empty());
}

TEST_CASE("Image: mat access on valid image", "[image]")
{
    Image img;
    REQUIRE(img.mat().empty());

    img.internal().mat = cv::Mat::zeros(10, 10, CV_8UC3);
    img.internal().info.width = 10;
    img.internal().info.height = 10;
    img.internal().info.pixel_format = PixelFormat::RGB8;

    REQUIRE_FALSE(img.mat().empty());
    REQUIRE(img.pixels() != nullptr);
    REQUIRE(img.info().width == 10);
}

TEST_CASE("Image: copy is deleted", "[image]")
{
    // Compile-time check: these should not compile.
    // We verify the type traits instead.
    REQUIRE_FALSE(std::is_copy_constructible_v<Image>);
    REQUIRE_FALSE(std::is_copy_assignable_v<Image>);
    REQUIRE(std::is_move_constructible_v<Image>);
    REQUIRE(std::is_move_assignable_v<Image>);
}
