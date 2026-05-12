// Unit tests for SelectionModel.
//
// SelectionModel owns the std::set<int> of selected entry indices and
// the user-controlled A/B swap toggle.  These tests cover:
//   * insert / erase / toggle / clear / replace semantics and their
//     return values (which callers use to decide cache invalidation)
//   * apply_remap correctness against ImageLibrary::kRemoved and
//     out-of-range indices, including the membership-changed signal
//   * get_ab_indices ordering and swap_ab interaction
//
// SelectionModel has no SDL/ImGui dependencies, so the tests exercise
// it directly with no fakes.

#include <catch2/catch_test_macros.hpp>

#include "domain/selection_model.h"
#include "domain/image_library.h"

#include <set>
#include <vector>

using idiff::ImageLibrary;
using idiff::SelectionModel;

TEST_CASE("SelectionModel: default state is empty with swap_ab=false",
          "[selection_model]") {
    SelectionModel sel;
    REQUIRE(sel.empty());
    REQUIRE(sel.size() == 0);
    REQUIRE_FALSE(sel.swap_ab());
    REQUIRE(sel.indices().empty());
}

TEST_CASE("SelectionModel: insert returns true only on first insert",
          "[selection_model]") {
    SelectionModel sel;
    REQUIRE(sel.insert(3));
    REQUIRE_FALSE(sel.insert(3));
    REQUIRE(sel.size() == 1);
    REQUIRE(sel.contains(3));
}

TEST_CASE("SelectionModel: erase returns true only when element existed",
          "[selection_model]") {
    SelectionModel sel;
    sel.insert(1);
    sel.insert(2);
    REQUIRE(sel.erase(1));
    REQUIRE_FALSE(sel.erase(1));
    REQUIRE(sel.size() == 1);
    REQUIRE_FALSE(sel.contains(1));
    REQUIRE(sel.contains(2));
}

TEST_CASE("SelectionModel: toggle flips membership and reports new state",
          "[selection_model]") {
    SelectionModel sel;
    REQUIRE(sel.toggle(5));
    REQUIRE(sel.contains(5));
    REQUIRE_FALSE(sel.toggle(5));
    REQUIRE_FALSE(sel.contains(5));
}

TEST_CASE("SelectionModel: clear empties the set but leaves swap_ab alone",
          "[selection_model]") {
    SelectionModel sel;
    sel.insert(1);
    sel.insert(2);
    sel.set_swap_ab(true);
    sel.clear();
    REQUIRE(sel.empty());
    // clear() is intentionally membership-only; callers reset swap_ab
    // explicitly when they want to.
    REQUIRE(sel.swap_ab());
}

TEST_CASE("SelectionModel: replace reports change only when set differs",
          "[selection_model]") {
    SelectionModel sel;
    sel.insert(1);
    sel.insert(2);

    REQUIRE_FALSE(sel.replace(std::set<int>{1, 2}));
    REQUIRE(sel.replace(std::set<int>{2, 3}));
    REQUIRE(sel.indices() == std::set<int>{2, 3});
}

TEST_CASE("SelectionModel: swap_ab toggle/setter are independent of indices",
          "[selection_model]") {
    SelectionModel sel;
    REQUIRE_FALSE(sel.swap_ab());
    sel.toggle_swap_ab();
    REQUIRE(sel.swap_ab());
    sel.set_swap_ab(false);
    REQUIRE_FALSE(sel.swap_ab());
}

TEST_CASE("SelectionModel: apply_remap drops kRemoved indices",
          "[selection_model]") {
    SelectionModel sel;
    sel.insert(0);
    sel.insert(1);
    sel.insert(2);

    // Remove old index 1; the others shift down by one.
    std::vector<int> remap = {0, ImageLibrary::kRemoved, 1};
    REQUIRE(sel.apply_remap(remap));
    REQUIRE(sel.indices() == std::set<int>{0, 1});
}

TEST_CASE("SelectionModel: apply_remap drops out-of-range indices",
          "[selection_model]") {
    SelectionModel sel;
    sel.insert(2);
    sel.insert(5);  // beyond the remap

    std::vector<int> remap = {0, 1, 2};
    REQUIRE(sel.apply_remap(remap));
    REQUIRE(sel.indices() == std::set<int>{2});
}

TEST_CASE("SelectionModel: apply_remap returns false on pure permutation",
          "[selection_model]") {
    SelectionModel sel;
    sel.insert(0);
    sel.insert(2);

    // Sort that swaps positions 0 and 2 (1 stays put).
    std::vector<int> remap = {2, 1, 0};
    REQUIRE_FALSE(sel.apply_remap(remap));
    REQUIRE(sel.indices() == std::set<int>{0, 2});
}

TEST_CASE("SelectionModel: apply_remap on empty remap is a no-op",
          "[selection_model]") {
    SelectionModel sel;
    sel.insert(1);
    REQUIRE_FALSE(sel.apply_remap(std::vector<int>{}));
    REQUIRE(sel.indices() == std::set<int>{1});
}

TEST_CASE("SelectionModel: get_ab_indices returns -1 for empty selection",
          "[selection_model]") {
    SelectionModel sel;
    int a = 99, b = 99;
    sel.get_ab_indices(a, b);
    REQUIRE(a == -1);
    REQUIRE(b == -1);
}

TEST_CASE("SelectionModel: get_ab_indices returns first selected as A only",
          "[selection_model]") {
    SelectionModel sel;
    sel.insert(7);
    int a = -1, b = -1;
    sel.get_ab_indices(a, b);
    REQUIRE(a == 7);
    REQUIRE(b == -1);
}

TEST_CASE("SelectionModel: get_ab_indices uses first two indices in order",
          "[selection_model]") {
    SelectionModel sel;
    // std::set iterates in increasing order; A = smallest, B = next.
    sel.insert(3);
    sel.insert(1);
    sel.insert(5);
    int a = -1, b = -1;
    sel.get_ab_indices(a, b);
    REQUIRE(a == 1);
    REQUIRE(b == 3);
}

TEST_CASE("SelectionModel: get_ab_indices honors swap_ab",
          "[selection_model]") {
    SelectionModel sel;
    sel.insert(1);
    sel.insert(2);
    sel.set_swap_ab(true);
    int a = -1, b = -1;
    sel.get_ab_indices(a, b);
    REQUIRE(a == 2);
    REQUIRE(b == 1);
}
