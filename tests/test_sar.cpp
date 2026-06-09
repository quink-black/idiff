#include <catch2/catch_test_macros.hpp>

#include "core/image.h"

using namespace idiff;

// =============================================================================
// ImageInfo::display_width / display_height
// =============================================================================

TEST_CASE("ImageInfo: display dimensions equal pixel dimensions when SAR unset",
          "[image][sar]")
{
    ImageInfo info;
    info.width = 640;
    info.height = 480;
    // sar_num/sar_den default to 0 (unset)
    CHECK(info.display_width() == 640);
    CHECK(info.display_height() == 480);
}

TEST_CASE("ImageInfo: display dimensions equal pixel dimensions when SAR is 1:1",
          "[image][sar]")
{
    ImageInfo info;
    info.width = 640;
    info.height = 480;
    info.sar_num = 1;
    info.sar_den = 1;
    CHECK(info.display_width() == 640);
    CHECK(info.display_height() == 480);
}

TEST_CASE("ImageInfo: display_width applies SAR 4:3 to 480x480 frame",
          "[image][sar]")
{
    // 480x480 with SAR 4:3 -> display 640x480
    ImageInfo info;
    info.width = 480;
    info.height = 480;
    info.sar_num = 4;
    info.sar_den = 3;
    CHECK(info.display_width() == 640);
    CHECK(info.display_height() == 480);
}

TEST_CASE("ImageInfo: display_width applies SAR 10:11 to 704x480 frame",
          "[image][sar]")
{
    // NTSC DV: 704x480 SAR 10:11 -> display 640x480
    ImageInfo info;
    info.width = 704;
    info.height = 480;
    info.sar_num = 10;
    info.sar_den = 11;
    CHECK(info.display_width() == 640);
    CHECK(info.display_height() == 480);
}

TEST_CASE("ImageInfo: display_width applies SAR 8:9 to 720x480 frame",
          "[image][sar]")
{
    // NTSC DV wide: 720x480 SAR 8:9 -> display 640x480
    ImageInfo info;
    info.width = 720;
    info.height = 480;
    info.sar_num = 8;
    info.sar_den = 9;
    CHECK(info.display_width() == 640);
    CHECK(info.display_height() == 480);
}

TEST_CASE("ImageInfo: display_width applies SAR 16:15 to 1440x1080 frame",
          "[image][sar]")
{
    // HDV 1080i: 1440x1080 SAR 16:15 -> display 1536x1080
    ImageInfo info;
    info.width = 1440;
    info.height = 1080;
    info.sar_num = 16;
    info.sar_den = 15;
    CHECK(info.display_width() == 1536);
    CHECK(info.display_height() == 1080);
}

TEST_CASE("ImageInfo: display_width rounds to nearest integer",
          "[image][sar]")
{
    // 481x480 SAR 4:3 -> 481 * 4 / 3 = 641.333... -> rounds to 641
    ImageInfo info;
    info.width = 481;
    info.height = 480;
    info.sar_num = 4;
    info.sar_den = 3;
    CHECK(info.display_width() == 641);
    CHECK(info.display_height() == 480);
}

TEST_CASE("ImageInfo: SAR with den=0 treated as unset",
          "[image][sar]")
{
    ImageInfo info;
    info.width = 480;
    info.height = 480;
    info.sar_num = 4;
    info.sar_den = 0;
    CHECK(info.display_width() == 480);
    CHECK(info.display_height() == 480);
}

TEST_CASE("ImageInfo: SAR with num=0 treated as unset",
          "[image][sar]")
{
    ImageInfo info;
    info.width = 480;
    info.height = 480;
    info.sar_num = 0;
    info.sar_den = 3;
    CHECK(info.display_width() == 480);
    CHECK(info.display_height() == 480);
}
