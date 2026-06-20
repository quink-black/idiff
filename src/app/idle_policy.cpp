#include "app/idle_policy.h"

#include <SDL_video.h>

namespace idiff {

namespace {
// Visible window: cap the loop at ~60 fps without busy-spinning.
constexpr int kActiveTimeoutMs = 16;
// Hidden window: wake only a few times a second to keep RPC and
// background polls responsive while the CPU stays near idle.
constexpr int kMinimizedTimeoutMs = 100;
} // namespace

int loop_wait_timeout_ms(bool minimized) {
    return minimized ? kMinimizedTimeoutMs : kActiveTimeoutMs;
}

bool loop_should_render(bool minimized) {
    return !minimized;
}

bool apply_window_event(bool minimized, std::uint32_t sdl_window_event) {
    switch (sdl_window_event) {
        case SDL_WINDOWEVENT_MINIMIZED:
        case SDL_WINDOWEVENT_HIDDEN:
            return true;
        case SDL_WINDOWEVENT_RESTORED:
        case SDL_WINDOWEVENT_SHOWN:
            return false;
        default:
            return minimized;
    }
}

} // namespace idiff
