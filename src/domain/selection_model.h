#ifndef IDIFF_DOMAIN_SELECTION_MODEL_H
#define IDIFF_DOMAIN_SELECTION_MODEL_H

// Selection model.
//
// Owns the set of selected entry indices.  Membership is stored as
// std::set<int> so iteration is always in increasing index order, which
// the comparison code relies on to decide which selected entry is the
// reference image (the smallest index).
//
// Threading: not thread-safe.  All operations must run on the main
// thread (the one driving ImGui).
//
// The model is intentionally container-only: it does not know about
// ImageLibrary, diff slots, or the viewport.  Callers tell it what
// changed; it does not call back into them.

#include <cstddef>
#include <set>
#include <vector>

namespace idiff {

class SelectionModel {
public:
    SelectionModel() = default;

    // Mutators.  Each mutation that can change membership returns
    // whether the membership actually changed; the caller uses this to
    // decide whether downstream caches (e.g. diff slots, reference-
    // derived texture flags) need invalidating.
    bool insert(int idx);     // returns true on first-time insert
    bool erase(int idx);      // returns true on actual removal
    void clear();
    bool toggle(int idx);     // returns the new membership state

    // Wholesale replace.  Returns true iff the new set differs from
    // the previous set (membership-only; permutation differences
    // cannot exist for std::set).
    bool replace(std::set<int> new_indices);

    // Apply an "old index -> new index" remap as produced by
    // ImageLibrary::remove / move / sort_with.  Indices mapped to
    // ImageLibrary::kRemoved (== -1) are dropped; out-of-range indices
    // are dropped.  Returns true iff membership changed.  An empty
    // remap is a no-op (returns false).
    bool apply_remap(const std::vector<int>& remap);

    // Read-only views.
    const std::set<int>& indices() const noexcept { return indices_; }
    std::size_t size() const noexcept { return indices_.size(); }
    bool empty() const noexcept { return indices_.empty(); }
    bool contains(int idx) const noexcept { return indices_.count(idx) > 0; }

    // Compute the reference entry index used by the comparison views.
    // The reference is the smallest selected index, or -1 when the
    // selection is empty.  Every other selected entry is a partner
    // compared against the reference (in natural index order).
    void get_ref_index(int& ref_idx) const noexcept;

private:
    std::set<int> indices_;
};

} // namespace idiff

#endif // IDIFF_DOMAIN_SELECTION_MODEL_H
