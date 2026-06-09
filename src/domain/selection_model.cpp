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
    bool removed = indices_.erase(idx) > 0;
    if (removed && reference_index_ == idx) reference_index_.reset();
    return removed;
}

void SelectionModel::clear() {
    indices_.clear();
    reference_index_.reset();
}

bool SelectionModel::toggle(int idx) {
    if (indices_.erase(idx) > 0) {
        if (reference_index_ == idx) reference_index_.reset();
        return false;
    }
    indices_.insert(idx);
    return true;
}

bool SelectionModel::replace(std::set<int> new_indices) {
    if (new_indices == indices_) return false;
    indices_ = std::move(new_indices);
    reference_index_.reset();
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

    // Remap the explicit reference index when one is set.
    if (reference_index_.has_value()) {
        const int old_ref = *reference_index_;
        if (old_ref < 0 || old_ref >= static_cast<int>(remap.size())
            || remap[old_ref] == ImageLibrary::kRemoved) {
            reference_index_.reset();
            dropped = true;
        } else {
            reference_index_ = remap[old_ref];
        }
    }

    const bool size_changed = remapped.size() != indices_.size();
    indices_ = std::move(remapped);
    // Two ways to lose membership: an entry was removed, or two old
    // indices collapsed onto the same new one (cannot happen with the
    // current ImageLibrary remaps but we conservatively detect it).
    return dropped || size_changed;
}

void SelectionModel::get_ref_index(int& ref_idx) const noexcept {
    if (reference_index_.has_value()) {
        ref_idx = *reference_index_;
        return;
    }
    ref_idx = indices_.empty() ? -1 : *indices_.begin();
}

void SelectionModel::set_reference(int idx) {
    if (idx < 0)
        reference_index_.reset();
    else
        reference_index_ = idx;
}

void SelectionModel::set_reference(std::optional<int> idx) {
    if (idx.has_value() && *idx < 0) idx.reset();
    reference_index_ = idx;
}

bool SelectionModel::has_explicit_reference() const noexcept {
    return reference_index_.has_value();
}

} // namespace idiff
