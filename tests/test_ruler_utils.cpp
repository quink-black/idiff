#include <catch2/catch_test_macros.hpp>

#include "core/ruler_utils.h"

using idiff::compute_nice_interval;

TEST_CASE("compute_nice_interval follows the 1-2-5 sequence at 1:1 zoom",
          "[ruler_utils]") {
    // At scale == 1, the interval must equal the smallest 1-2-5 value that
    // is >= min_screen_spacing.
    REQUIRE(compute_nice_interval(1.0f, 1.0f) == 1);
    REQUIRE(compute_nice_interval(1.0f, 2.0f) == 2);
    REQUIRE(compute_nice_interval(1.0f, 3.0f) == 5);
    REQUIRE(compute_nice_interval(1.0f, 5.0f) == 5);
    REQUIRE(compute_nice_interval(1.0f, 6.0f) == 10);
    REQUIRE(compute_nice_interval(1.0f, 10.0f) == 10);
    REQUIRE(compute_nice_interval(1.0f, 11.0f) == 20);
    REQUIRE(compute_nice_interval(1.0f, 21.0f) == 50);
    REQUIRE(compute_nice_interval(1.0f, 51.0f) == 100);
}

TEST_CASE("compute_nice_interval scales inversely with zoom",
          "[ruler_utils]") {
    // When zoomed in by 10x, we can afford a 10x finer grid.
    REQUIRE(compute_nice_interval(10.0f, 50.0f) == 5);
    REQUIRE(compute_nice_interval(10.0f, 20.0f) == 2);
    REQUIRE(compute_nice_interval(10.0f, 10.0f) == 1);

    // When zoomed out to 0.1x, intervals need to be 10x coarser.
    REQUIRE(compute_nice_interval(0.1f, 50.0f) == 500);
    REQUIRE(compute_nice_interval(0.1f, 100.0f) == 1000);
}

TEST_CASE("compute_nice_interval returns the smallest qualifying interval",
          "[ruler_utils]") {
    // Scale = 1, min_spacing = 50 -> interval * 1 >= 50, smallest 1-2-5
    // candidate is 50.
    REQUIRE(compute_nice_interval(1.0f, 50.0f) == 50);
    // Just past the boundary: we need the next candidate up (100).
    REQUIRE(compute_nice_interval(1.0f, 50.1f) == 100);
}

TEST_CASE("compute_nice_interval result is always positive",
          "[ruler_utils]") {
    // Sweep through a range of zooms and spacings; output must be >= 1.
    for (float scale : {0.05f, 0.5f, 1.0f, 2.5f, 16.0f, 64.0f}) {
        for (float spacing : {1.0f, 50.0f, 200.0f}) {
            int interval = compute_nice_interval(scale, spacing);
            INFO("scale=" << scale << " spacing=" << spacing);
            REQUIRE(interval >= 1);
            // On-screen size of one interval must meet the minimum.
            REQUIRE(static_cast<float>(interval) * scale >= spacing);
        }
    }
}
