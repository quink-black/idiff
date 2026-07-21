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
    static constexpr std::size_t kCapacity = 4;

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
        pos_.clear();
        for (int old_idx : order_) {
            if (old_idx < 0 || static_cast<std::size_t>(old_idx) >= remap.size()) {
                // Index outside the remap range; keep as-is.
                new_order.push_back(old_idx);
                pos_[old_idx] = std::prev(new_order.end());
                continue;
            }
            int new_idx = remap[static_cast<std::size_t>(old_idx)];
            if (new_idx == kRemoved) continue;
            new_order.push_back(new_idx);
            pos_[new_idx] = std::prev(new_order.end());
        }
        order_ = std::move(new_order);
    }

    // Evict from the least-recently-used end until size <= kCapacity.
    // `evict_cb(index)` is invoked once per evicted index, in
    // oldest-first order.  The callback is responsible for releasing
    // the entry's pixels and GPU texture.
    template <class F>
    void evict_excess(F&& evict_cb) {
        while (order_.size() > kCapacity) {
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
    std::list<int> order_;
    std::unordered_map<int, std::list<int>::iterator> pos_;
};

} // namespace idiff

#endif // IDIFF_DOMAIN_LAZY_LOAD_CACHE_H
