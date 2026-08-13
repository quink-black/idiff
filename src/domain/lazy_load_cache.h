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
#include <limits>
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
    std::size_t byte_budget() const noexcept { return byte_budget_; }
    std::size_t resident_bytes() const noexcept { return resident_bytes_; }

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
            return;
        }
        touch(index, 0);
    }

    void touch(int index, std::size_t bytes) {
        auto it = pos_.find(index);
        if (it != pos_.end()) {
            resident_bytes_ -= weights_[index];
            weights_[index] = bytes;
            resident_bytes_ += bytes;
            order_.splice(order_.begin(), order_, it->second);
        } else {
            order_.push_front(index);
            pos_[index] = order_.begin();
            weights_[index] = bytes;
            resident_bytes_ += bytes;
        }
    }

    // Remove `index` from the cache without releasing anything.
    // No-op when the index is not cached.
    void remove(int index) {
        auto it = pos_.find(index);
        if (it == pos_.end()) return;
        resident_bytes_ -= weights_[index];
        weights_.erase(index);
        order_.erase(it->second);
        pos_.erase(it);
    }

    void clear() {
        order_.clear();
        pos_.clear();
        weights_.clear();
        resident_bytes_ = 0;
    }

    // Apply an "old index -> new index" remap (e.g. produced by
    // ImageLibrary::remove).  Indices mapped to kRemoved are dropped;
    // out-of-range targets are dropped.  Relative LRU order is
    // preserved.  An empty remap is a no-op.
    void apply_remap(const std::vector<int>& remap) {
        if (remap.empty()) return;
        std::list<int> new_order;
        std::unordered_map<int, std::size_t> new_weights;
        std::size_t new_resident_bytes = 0;
        for (int old_idx : order_) {
            int new_idx = old_idx;
            if (old_idx < 0 || static_cast<std::size_t>(old_idx) >= remap.size()) {
                new_idx = old_idx;
            } else {
                new_idx = remap[static_cast<std::size_t>(old_idx)];
                if (new_idx == kRemoved) continue;
            }

            // A malformed remap must not create duplicate cache entries.
            if (new_weights.find(new_idx) != new_weights.end()) continue;
            new_order.push_back(new_idx);
            new_weights[new_idx] = weights_[old_idx];
            new_resident_bytes += weights_[old_idx];
        }
        order_ = std::move(new_order);
        weights_ = std::move(new_weights);
        resident_bytes_ = new_resident_bytes;
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
        while (!order_.empty() &&
               (order_.size() > capacity_ || resident_bytes_ > byte_budget_)) {
            int idx = order_.back();
            order_.pop_back();
            pos_.erase(idx);
            resident_bytes_ -= weights_[idx];
            weights_.erase(idx);
            evict_cb(idx);
        }
    }

    template <class F>
    void set_byte_budget(std::size_t bytes, F&& evict_cb) {
        byte_budget_ = bytes;
        evict_excess(std::forward<F>(evict_cb));
    }

    bool contains(int index) const {
        return pos_.find(index) != pos_.end();
    }

    std::size_t size() const noexcept { return order_.size(); }

private:
    std::size_t capacity_;
    std::size_t byte_budget_ = std::numeric_limits<std::size_t>::max();
    std::size_t resident_bytes_ = 0;
    std::list<int> order_;
    std::unordered_map<int, std::list<int>::iterator> pos_;
    std::unordered_map<int, std::size_t> weights_;
};

} // namespace idiff

#endif // IDIFF_DOMAIN_LAZY_LOAD_CACHE_H
