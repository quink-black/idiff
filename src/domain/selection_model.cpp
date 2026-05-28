#include "domain/selection_model.h"

#include "domain/image_library.h"

#include <utility>

namespace idiff {

bool SelectionModel::insert(int idx) {
    auto [it, inserted] = indices_.insert(idx);
    (void)it;
    return inserted;
}

bool SelectionModel::erase(int idx) {
    return indices_.erase(idx) > 0;
}

void SelectionModel::clear() {
    indices_.clear();
}

bool SelectionModel::toggle(int idx) {
    if (indices_.erase(idx) > 0) return false;
    indices_.insert(idx);
    return true;
}

bool SelectionModel::replace(std::set<int> new_indices) {
    if (new_indices == indices_) return false;
    indices_ = std::move(new_indices);
    return true;
}

bool SelectionModel::apply_remap(const std::vector<int>& remap) {
    if (remap.empty()) return false;

    std::set<int> remapped;
    bool dropped = false;
    for (int s : indices_) {
        if (s < 0 || s >= static_cast<int>(remap.size())) {
            dropped = true;
            continue;
        }
        const int n = remap[s];
        if (n == ImageLibrary::kRemoved) {
            dropped = true;
            continue;
        }
        remapped.insert(n);
    }

    const bool size_changed = remapped.size() != indices_.size();
    indices_ = std::move(remapped);
    // Two ways to lose membership: an entry was removed, or two old
    // indices collapsed onto the same new one (cannot happen with the
    // current ImageLibrary remaps but we conservatively detect it).
    return dropped || size_changed;
}

void SelectionModel::get_ref_index(int& ref_idx) const noexcept {
    ref_idx = indices_.empty() ? -1 : *indices_.begin();
}

} // namespace idiff
