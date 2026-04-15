#include <catch2/catch_test_macros.hpp>

#include "core/image_loader.h"
#include "core/detail/raw_loader.h"

using namespace idiff;

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
