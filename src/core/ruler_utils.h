#ifndef IDIFF_CORE_RULER_UTILS_H
#define IDIFF_CORE_RULER_UTILS_H

namespace idiff {

// Find a "nice" tick interval (in image pixels) such that ticks are at
// least `min_screen_spacing` pixels apart on screen, using the classic
// 1-2-5 sequence (1, 2, 5, 10, 20, 50, 100, ...).
//
// Parameters:
//   scale               - display pixels per image pixel (> 0)
//   min_screen_spacing  - minimum desired spacing between ticks in screen
//                         pixels (> 0)
//
// Returns: the smallest interval from the 1-2-5 sequence whose on-screen
// size is at least `min_screen_spacing`.  Always returns >= 1.
//
// This is a pure function (no side effects, no globals) and is safe to
// call from any thread.  It is kept header-only so it can be linked into
// both the GUI target and the unit-test target without pulling in ImGui.
inline int compute_nice_interval(float scale, float min_screen_spacing) {
    static const int bases[] = {1, 2, 5};
    int magnitude = 1;
    // Cap magnitude to avoid pathological infinite loops when callers
    // pass non-positive scale.  At 1e9 pixels per image pixel we've
    // clearly saturated; return the last candidate.
    while (magnitude <= 1000000000) {
        for (int b : bases) {
            int interval = b * magnitude;
            if (static_cast<float>(interval) * scale >= min_screen_spacing) {
                return interval;
            }
        }
        magnitude *= 10;
    }
    return magnitude;
}

} // namespace idiff

#endif // IDIFF_CORE_RULER_UTILS_H
