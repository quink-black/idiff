#include <catch2/catch_test_macros.hpp>

#include "domain/group_key.h"

#include <string>

using namespace idiff;

TEST_CASE("group_key_from_filename basic cases") {
    REQUIRE(group_key_from_filename("kid.jpg") == "kid");
    REQUIRE(group_key_from_filename("kid.pisa.jpg") == "kid.pisa");
    REQUIRE(group_key_from_filename("noext") == "noext");
    REQUIRE(group_key_from_filename(".hidden") == ".hidden");
    REQUIRE(group_key_from_filename("a.b.c.png") == "a.b.c");
}

TEST_CASE("group_key_from_filename edge cases") {
    REQUIRE(group_key_from_filename("") == "");
    REQUIRE(group_key_from_filename("x") == "x");
    REQUIRE(group_key_from_filename(".") == ".");
    REQUIRE(group_key_from_filename("..") == ".");
    REQUIRE(group_key_from_filename("foo.") == "foo");
}

TEST_CASE("split_stem_ext") {
    auto [s, e] = split_stem_ext("kid.jpg");
    REQUIRE(s == "kid");
    REQUIRE(e == "jpg");

    auto [s2, e2] = split_stem_ext("noext");
    REQUIRE(s2 == "noext");
    REQUIRE(e2.empty());

    auto [s3, e3] = split_stem_ext(".hidden");
    REQUIRE(s3 == ".hidden");
    REQUIRE(e3.empty());

    auto [s4, e4] = split_stem_ext("a.b.c.png");
    REQUIRE(s4 == "a.b.c");
    REQUIRE(e4 == "png");
}

TEST_CASE("group keys match for same-stem files from different directories") {
    // The function only looks at the filename, not the path.
    // Caller is responsible for passing just the filename portion.
    REQUIRE(group_key_from_filename("foo.jpg") ==
            group_key_from_filename("foo.png"));
    REQUIRE(group_key_from_filename("foo.jpg") !=
            group_key_from_filename("bar.jpg"));
}
