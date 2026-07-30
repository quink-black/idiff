#ifndef IDIFF_APP_GEOM_UTILS_H
#define IDIFF_APP_GEOM_UTILS_H

#include <algorithm>

namespace idiff {

// Clamp `val` into [lo, hi]. std::clamp is undefined behavior when lo > hi,
// which happens in label placement whenever the box is wider than the cell it
// must stay inside. clamp_safe degrades gracefully: when the interval is
// inverted it returns lo, pinning the box to the start edge instead of
// crashing. This is the correct readable default for content that does not fit.
template <typename T>
T clamp_safe(T val, T lo, T hi) {
    if (lo > hi) return lo;
    return val < lo ? lo : (val > hi ? hi : val);
}

}  // namespace idiff

#endif  // IDIFF_APP_GEOM_UTILS_H
