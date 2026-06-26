#ifndef IDIFF_IDLE_POLICY_H
#define IDIFF_IDLE_POLICY_H

#include <chrono>
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

// ---------------------------------------------------------------------------
// IdleTracker -- extends the minimized-only policy with user / MCP idle
// detection.  When the window is visible but no user input or MCP
// requests have arrived for a configurable grace period, rendering is
// skipped entirely and the event-loop timeout is lengthened so the
// thread sleeps deeper.  Any user interaction or MCP activity wakes it
// back up immediately.
//
// Three states:
//   Active    -- recent interaction, render every frame (16 ms timeout).
//   Idle      -- no interaction for kIdleGrace, skip render, long timeout.
//   Minimized -- window hidden, skip render, deep-sleep timeout.
//
// Thread safety: all methods are intended to be called from the main
// thread only (matching the SDL / App threading model).
// ---------------------------------------------------------------------------

class IdleTracker {
public:
    // Classification of an SDL event consumed by the main loop.  Used
    // by on_sdl_event() to decide whether the event counts as user
    // interaction that should wake the app from idle.
    enum class EventClass {
        UserInput,   // mouse, keyboard, text, drop -- breaks idle
        Window,      // minimize/restore/show/hide -- changes minimized
        Other,       // timer, clipboard, etc. -- does not break idle
    };

    explicit IdleTracker(
        std::chrono::steady_clock::duration idle_grace =
            std::chrono::seconds(5));

    // Reset the "last user activity" timestamp to now.  Call when a
    // user-input SDL event is processed.
    void on_user_input();

    // Reset the "last MCP activity" timestamp to now.  Call after
    // rpc_server_->drain() returns > 0 (at least one request was
    // dispatched).
    void on_mcp_activity();

    // Classify a raw SDL event type and update internal state.  Returns
    // the classification so the main loop can apply_window_event() on
    // Window events.  UserInput events implicitly call on_user_input().
    EventClass on_sdl_event(std::uint32_t sdl_event_type,
                            std::uint32_t sdl_window_event = 0);

    // Update the minimized flag from a window event.  Should be called
    // with the result of apply_window_event().
    void set_minimized(bool minimized);

    // Current loop parameters -- the main loop calls these each
    // iteration instead of the free functions above.
    int wait_timeout_ms() const;
    bool should_render() const;

    // True when the tracker is in Idle or Minimized state (render is
    // skipped).  Useful for deciding whether to call tick_idle()
    // instead of frame().
    bool is_idle() const;

    // Exposed for testing.
    bool is_minimized() const noexcept { return minimized_; }

private:
    bool minimized_ = false;
    std::chrono::steady_clock::duration idle_grace_;
    std::chrono::steady_clock::time_point last_user_input_;
    std::chrono::steady_clock::time_point last_mcp_input_;
};

// Classify a raw SDL event type into EventClass without touching any
// IdleTracker state.  Useful for unit testing the classification
// independently.
IdleTracker::EventClass classify_sdl_event(std::uint32_t sdl_event_type);

} // namespace idiff

#endif // IDIFF_IDLE_POLICY_H
