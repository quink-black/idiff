#ifndef IDIFF_DOMAIN_SELECTION_MODEL_H
#define IDIFF_DOMAIN_SELECTION_MODEL_H

// Selection model.
//
// Owns the set of selected entry indices and the user-controlled "swap
// A/B" toggle.  Membership is stored as std::set<int> so iteration is
// always in increasing index order, which the comparison code relies
// on to decide which selected entry is "A" (the smallest index, modulo
// the swap flag).
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
    // decide whether downstream caches (e.g. diff slots, A/B-derived
    // texture flags) need invalidating, and whether the swap_ab flag
    // should be reset because the pair it referred to has changed.
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
    // are dropped.  Returns true iff membership changed (so callers
    // can decide whether to also reset swap_ab_).  An empty remap is
    // a no-op (returns false).
    bool apply_remap(const std::vector<int>& remap);

    // A/B swap toggle.  Independent of the index set; reset to false
    // by callers whenever membership changes meaningfully (the new
    // pair has no relationship to the old "B is on top" choice).
    bool swap_ab() const noexcept { return swap_ab_; }
    void set_swap_ab(bool v) noexcept { swap_ab_ = v; }
    void toggle_swap_ab() noexcept { swap_ab_ = !swap_ab_; }

    // Read-only views.
    const std::set<int>& indices() const noexcept { return indices_; }
    std::size_t size() const noexcept { return indices_.size(); }
    bool empty() const noexcept { return indices_.empty(); }
    bool contains(int idx) const noexcept { return indices_.count(idx) > 0; }

    // Compute the A and B entry indices the comparison views render.
    // A is the first selected index (or -1 when nothing is selected);
    // B is the second (or -1 when only one is selected).  The swap_ab
    // flag, if set, swaps the two before returning.  Caller passes
    // out-parameters by reference to mirror the previous App helper.
    void get_ab_indices(int& a_idx, int& b_idx) const noexcept;

private:
    std::set<int> indices_;
    bool swap_ab_ = false;
};

} // namespace idiff

#endif // IDIFF_DOMAIN_SELECTION_MODEL_H
