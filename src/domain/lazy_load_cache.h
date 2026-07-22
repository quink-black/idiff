#ifndef IDIFF_DOMAIN_LAZY_LOAD_CACHE_H
#define IDIFF_DOMAIN_LAZY_LOAD_CACHE_H

// Fixed-capacity LRU of entry indices that have recently left the
// selection.  The cache owns no pixel data itself; it only records the
// order so AppController can decide which entries to evict (release
// pixels + GPU texture) when the cache exceeds kCapacity.  Selection
// members are never present in the cache -- they are removed when they
// enter the selection and inserted when they leave.
//
// Threading: not thread-safe; main thread only, same as ImageLibrary.
//
// Header-only: no SDL / Image dependency, just standard containers.

#include <cstddef>
#include <list>
#include <unordered_map>
#include <vector>

namespace idiff {

class LazyLoadCache {
public:
    static constexpr int kRemoved = -1;

    // Default capacity: 20 entries.  At ~33 MB per 4K RGBA8 image
    // this caps the LRU near ~660 MB for the largest common images;
    // for 1080p (~8 MB each) it sits around ~160 MB.  The user can
    // tune this via the settings file.
    static constexpr std::size_t kDefaultCapacity = 20;

    explicit LazyLoadCache(std::size_t capacity = kDefaultCapacity)
        : capacity_(capacity) {}

    std::size_t capacity() const noexcept { return capacity_; }

    // Change the capacity at runtime.  If the new capacity is smaller
    // than the current size, excess entries are evicted immediately
    // via evict_excess().
    template <class F>
    void set_capacity(std::size_t new_capacity, F&& evict_cb) {
        capacity_ = new_capacity;
        evict_excess(std::forward<F>(evict_cb));
    }

    // Promote `index` to the most-recently-used position.  If the
    // index is already cached, it is moved to the front; otherwise it
    // is inserted at the front.
    void touch(int index) {
        auto it = pos_.find(index);
        if (it != pos_.end()) {
            order_.splice(order_.begin(), order_, it->second);
        } else {
            order_.push_front(index);
            pos_[index] = order_.begin();
        }
    }

    // Remove `index` from the cache without releasing anything.
    // No-op when the index is not cached.
    void remove(int index) {
        auto it = pos_.find(index);
        if (it == pos_.end()) return;
        order_.erase(it->second);
        pos_.erase(it);
    }

    void clear() {
        order_.clear();
        pos_.clear();
    }

    // Apply an "old index -> new index" remap (e.g. produced by
    // ImageLibrary::remove).  Indices mapped to kRemoved are dropped;
    // out-of-range targets are dropped.  Relative LRU order is
    // preserved.  An empty remap is a no-op.
    void apply_remap(const std::vector<int>& remap) {
        if (remap.empty()) return;
        std::list<int> new_order;
        for (int old_idx : order_) {
            if (old_idx < 0 || static_cast<std::size_t>(old_idx) >= remap.size()) {
                // Index outside the remap range; keep as-is.
                new_order.push_back(old_idx);
                continue;
            }
            int new_idx = remap[static_cast<std::size_t>(old_idx)];
            if (new_idx == kRemoved) continue;
            new_order.push_back(new_idx);
        }
        order_ = std::move(new_order);
        // Rebuild pos_ from the final order_ so iterators are valid.
        pos_.clear();
        for (auto it = order_.begin(); it != order_.end(); ++it) {
            pos_[*it] = it;
        }
    }

    // Evict from the least-recently-used end until size <= capacity_.
    // `evict_cb(index)` is invoked once per evicted index, in
    // oldest-first order.  The callback is responsible for releasing
    // the entry's pixels and GPU texture.
    template <class F>
    void evict_excess(F&& evict_cb) {
        while (order_.size() > capacity_) {
            int idx = order_.back();
            order_.pop_back();
            pos_.erase(idx);
            evict_cb(idx);
        }
    }

    bool contains(int index) const {
        return pos_.find(index) != pos_.end();
    }

    std::size_t size() const noexcept { return order_.size(); }

private:
    std::size_t capacity_;
    std::list<int> order_;
    std::unordered_map<int, std::list<int>::iterator> pos_;
};

} // namespace idiff

#endif // IDIFF_DOMAIN_LAZY_LOAD_CACHE_H
