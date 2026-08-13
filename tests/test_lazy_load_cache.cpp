// Unit tests for LazyLoadCache.
//
// LazyLoadCache is a configurable-capacity LRU of entry indices that
// recently left the selection.  These tests cover:
//   * touch / remove / contains / size semantics
//   * LRU ordering: most-recently-touched at front
//   * evict_excess fires the callback in oldest-first order and only
//     for entries beyond capacity
//   * apply_remap preserves relative order and drops kRemoved indices

#include <catch2/catch_test_macros.hpp>

#include "domain/lazy_load_cache.h"

#include <vector>

namespace {

// Small capacity for testing (4, matching the old fixed default).
constexpr std::size_t kTestCap = 4;

} // namespace

TEST_CASE("LazyLoadCache starts empty", "[lazy_load]") {
    idiff::LazyLoadCache c{kTestCap};
    REQUIRE(c.size() == 0);
    REQUIRE_FALSE(c.contains(0));
}

TEST_CASE("LazyLoadCache::touch inserts at front", "[lazy_load]") {
    idiff::LazyLoadCache c{kTestCap};
    c.touch(1);
    c.touch(2);
    c.touch(3);
    REQUIRE(c.size() == 3);
    REQUIRE(c.contains(1));
    REQUIRE(c.contains(2));
    REQUIRE(c.contains(3));
}

TEST_CASE("LazyLoadCache::touch promotes existing to front", "[lazy_load]") {
    idiff::LazyLoadCache c{kTestCap};
    c.touch(1);
    c.touch(2);
    c.touch(3);
    // Order (front to back): 3, 2, 1
    c.touch(1);
    // Order: 1, 3, 2
    c.touch(2);
    // Order: 2, 1, 3
    c.touch(4);
    // Order: 4, 2, 1, 3 (size == capacity, no eviction yet)

    std::vector<int> evicted;
    c.evict_excess([&](int idx) { evicted.push_back(idx); });
    REQUIRE(evicted.empty());

    c.touch(5);
    // Order: 5, 4, 2, 1, 3 (size 5, one over capacity)
    c.evict_excess([&](int idx) { evicted.push_back(idx); });
    REQUIRE(evicted.size() == 1);
    REQUIRE(evicted[0] == 3);  // oldest gets evicted
    REQUIRE(c.size() == kTestCap);
    REQUIRE_FALSE(c.contains(3));
}

TEST_CASE("LazyLoadCache::remove drops an entry without evicting",
          "[lazy_load]") {
    idiff::LazyLoadCache c{kTestCap};
    c.touch(1);
    c.touch(2);
    c.touch(3);

    std::vector<int> evicted;
    c.evict_excess([&](int idx) { evicted.push_back(idx); });
    REQUIRE(evicted.empty());

    c.remove(2);
    REQUIRE(c.size() == 2);
    REQUIRE_FALSE(c.contains(2));
    REQUIRE(c.contains(1));
    REQUIRE(c.contains(3));
}

TEST_CASE("LazyLoadCache::evict_excess fires oldest-first at capacity",
          "[lazy_load]") {
    idiff::LazyLoadCache c{kTestCap};
    // Fill exactly to capacity.  Order (front to back): 40, 30, 20, 10
    c.touch(10);
    c.touch(20);
    c.touch(30);
    c.touch(40);
    REQUIRE(c.size() == kTestCap);

    std::vector<int> evicted;
    c.evict_excess([&](int idx) { evicted.push_back(idx); });
    // At capacity -- no eviction yet.
    REQUIRE(evicted.empty());

    // Touch one more -- the oldest (10) should evict.
    c.touch(50);
    // Order: 50, 40, 30, 20, 10 (size 5)
    c.evict_excess([&](int idx) { evicted.push_back(idx); });
    REQUIRE(evicted.size() == 1);
    REQUIRE(evicted[0] == 10);
    REQUIRE_FALSE(c.contains(10));
    REQUIRE(c.size() == kTestCap);

    // Touch two more in a row -- 20 then 30 should evict in order.
    c.touch(60);
    c.evict_excess([&](int idx) { evicted.push_back(idx); });
    c.touch(70);
    c.evict_excess([&](int idx) { evicted.push_back(idx); });
    REQUIRE(evicted.size() == 3);
    REQUIRE(evicted[1] == 20);
    REQUIRE(evicted[2] == 30);
}

TEST_CASE("LazyLoadCache::clear empties the cache", "[lazy_load]") {
    idiff::LazyLoadCache c{kTestCap};
    c.touch(1);
    c.touch(2);
    c.clear();
    REQUIRE(c.size() == 0);
    REQUIRE_FALSE(c.contains(1));
    REQUIRE_FALSE(c.contains(2));
}

TEST_CASE("LazyLoadCache::apply_remap preserves order and drops removed",
          "[lazy_load]") {
    idiff::LazyLoadCache c{kTestCap};
    // Build order.  Each touch inserts/promotes to front, so after
    //   touch(3); touch(1); touch(4); touch(2);
    // front-to-back is: 2, 4, 1, 3
    c.touch(3);
    c.touch(1);
    c.touch(4);
    c.touch(2);

    // Remap: index 1 is removed, 2->5, 3->2, 4->4.
    // Pre-remap front-to-back: 2, 4, 1, 3.
    // Post-remap (drop 1, apply 2->5, 4->4, 3->2): 5, 4, 2.
    std::vector<int> remap(5);
    remap[0] = 0;  // untouched
    remap[1] = idiff::LazyLoadCache::kRemoved;
    remap[2] = 5;
    remap[3] = 2;
    remap[4] = 4;
    c.apply_remap(remap);

    REQUIRE(c.size() == 3);
    REQUIRE_FALSE(c.contains(1));
    REQUIRE(c.contains(2));   // 3 -> 2
    REQUIRE(c.contains(4));   // 4 -> 4
    REQUIRE(c.contains(5));   // 2 -> 5

    // Verify order: front-to-back is 5, 4, 2 so the back (2) is the
    // oldest and should be evicted first when the cache overflows.
    std::vector<int> evicted;
    c.touch(99);  // size 4, no evict yet
    c.evict_excess([&](int idx) { evicted.push_back(idx); });
    REQUIRE(evicted.empty());

    c.touch(100);  // size 5, evict back (2)
    c.evict_excess([&](int idx) { evicted.push_back(idx); });
    REQUIRE(evicted.size() == 1);
    REQUIRE(evicted[0] == 2);
}

TEST_CASE("LazyLoadCache::apply_remap with empty remap is a no-op",
          "[lazy_load]") {
    idiff::LazyLoadCache c{kTestCap};
    c.touch(1);
    c.touch(2);
    c.apply_remap({});
    REQUIRE(c.size() == 2);
    REQUIRE(c.contains(1));
    REQUIRE(c.contains(2));
}

TEST_CASE("LazyLoadCache::set_capacity shrinks and evicts immediately",
          "[lazy_load]") {
    idiff::LazyLoadCache c{10};
    for (int i = 0; i < 10; ++i) c.touch(i);
    // Order (front to back): 9, 8, 7, ..., 0
    REQUIRE(c.size() == 10);

    std::vector<int> evicted;
    c.set_capacity(3, [&](int idx) { evicted.push_back(idx); });
    REQUIRE(c.capacity() == 3);
    REQUIRE(c.size() == 3);
    // Oldest 7 entries (0..6) should be evicted in that order.
    REQUIRE(evicted.size() == 7);
    for (int i = 0; i < 7; ++i) {
        REQUIRE(evicted[static_cast<std::size_t>(i)] == i);
    }
    // The 3 most-recently-touched entries survive.
    REQUIRE(c.contains(9));
    REQUIRE(c.contains(8));
    REQUIRE(c.contains(7));
}

TEST_CASE("LazyLoadCache::set_capacity grows without eviction",
          "[lazy_load]") {
    idiff::LazyLoadCache c{4};
    for (int i = 0; i < 4; ++i) c.touch(i);
    std::vector<int> evicted;
    c.set_capacity(20, [&](int idx) { evicted.push_back(idx); });
    REQUIRE(evicted.empty());
    REQUIRE(c.capacity() == 20);
    REQUIRE(c.size() == 4);
}

TEST_CASE("LazyLoadCache evicts by resident byte budget", "[lazy_load]") {
    idiff::LazyLoadCache c{10};
    c.touch(1, 40);
    c.touch(2, 50);
    c.touch(3, 60);
    REQUIRE(c.resident_bytes() == 150);

    std::vector<int> evicted;
    c.set_byte_budget(100, [&](int idx) { evicted.push_back(idx); });

    REQUIRE(evicted == std::vector<int>{1, 2});
    REQUIRE(c.resident_bytes() == 60);
    REQUIRE(c.contains(3));
}

TEST_CASE("LazyLoadCache remap preserves resident byte accounting",
          "[lazy_load]") {
    idiff::LazyLoadCache c{10};
    c.touch(1, 25);
    c.touch(2, 75);
    std::vector<int> remap = {0, idiff::LazyLoadCache::kRemoved, 5};
    c.apply_remap(remap);

    REQUIRE(c.resident_bytes() == 75);
    REQUIRE_FALSE(c.contains(1));
    REQUIRE(c.contains(5));
}

TEST_CASE("LazyLoadCache unweighted touch preserves an existing weight",
          "[lazy_load]") {
    idiff::LazyLoadCache c{10};
    c.touch(1, 75);
    c.touch(2, 25);

    c.touch(1);

    REQUIRE(c.resident_bytes() == 100);
    std::vector<int> evicted;
    c.set_byte_budget(80, [&](int idx) { evicted.push_back(idx); });
    REQUIRE(evicted == std::vector<int>{2});
    REQUIRE(c.contains(1));
}

TEST_CASE("LazyLoadCache drops duplicate remap targets", "[lazy_load]") {
    idiff::LazyLoadCache c{10};
    c.touch(1, 25);
    c.touch(2, 75);

    c.apply_remap({0, 5, 5});

    REQUIRE(c.size() == 1);
    REQUIRE(c.contains(5));
    REQUIRE(c.resident_bytes() == 75);
}
