#ifndef IDIFF_IDLE_POLICY_H
#define IDIFF_IDLE_POLICY_H

#include <cstdint>

namespace idiff {

// Event-loop power policy, extracted from the main loop so the
// "do not busy-spin, do not render a hidden window" guarantees can be
// unit tested without SDL, ImGui, or a live window.  The values here
// are the load-bearing part of idle CPU usage: a regression that drops
// the wait timeout to 0 or renders while minimized would silently bring
// back the ~8% idle burn this policy exists to prevent.

// Milliseconds the event wait should block for, given whether the
// window is currently minimized or hidden.  Always > 0 so the loop
// sleeps rather than spins, and the minimized timeout is never shorter
// than the visible one (a hidden window should idle at least as deeply
// as a visible one).
int loop_wait_timeout_ms(bool minimized);

// Whether the full render pass should run this loop iteration.  False
// while minimized so a hidden window does no ImGui / present work.
bool loop_should_render(bool minimized);

// Fold an SDL window-event subtype (the `event.window.event` byte of an
// SDL_WINDOWEVENT) into the minimized flag, returning the new value.
// MINIMIZED / HIDDEN enter the low-power state; RESTORED / SHOWN leave
// it.  Every other subtype -- notably FOCUS_LOST, which must not be
// mistaken for minimize or a backgrounded-but-visible window would stop
// rendering -- leaves the flag unchanged.  The parameter is the raw
// SDL enum widened to uint32_t so this header pulls in no SDL include.
bool apply_window_event(bool minimized, std::uint32_t sdl_window_event);

} // namespace idiff

#endif // IDIFF_IDLE_POLICY_H
