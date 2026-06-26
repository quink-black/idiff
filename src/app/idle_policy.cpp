#include "app/idle_policy.h"

#include <SDL_video.h>

namespace idiff {

namespace {
// Visible window: cap the loop at ~60 fps without busy-spinning.
constexpr int kActiveTimeoutMs = 16;
// Hidden window: wake ~2 Hz to poll file watcher and SR tasks;
// all "needs immediate response" paths (user input, MCP, window
// restore) have their own wake-up so this is purely a fallback.
constexpr int kMinimizedTimeoutMs = 500;
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

// ---------------------------------------------------------------------------
// IdleTracker
// ---------------------------------------------------------------------------

IdleTracker::IdleTracker(std::chrono::steady_clock::duration idle_grace)
    : idle_grace_(idle_grace)
    , last_user_input_(std::chrono::steady_clock::now())
    , last_mcp_input_(std::chrono::steady_clock::now())
{}

void IdleTracker::on_user_input() {
    last_user_input_ = std::chrono::steady_clock::now();
}

void IdleTracker::on_mcp_activity() {
    last_mcp_input_ = std::chrono::steady_clock::now();
}

IdleTracker::EventClass IdleTracker::on_sdl_event(
        std::uint32_t sdl_event_type,
        std::uint32_t sdl_window_event) {
    EventClass cls = classify_sdl_event(sdl_event_type);

    if (cls == EventClass::UserInput) {
        on_user_input();
    }

    (void)sdl_window_event; // not used by on_sdl_event itself
    return cls;
}

void IdleTracker::set_minimized(bool minimized) {
    minimized_ = minimized;
    // Restoring from minimized counts as user activity so the first
    // frame after restore renders immediately instead of sitting in
    // Idle for another grace period.
    if (!minimized) {
        on_user_input();
    }
}

int IdleTracker::wait_timeout_ms() const {
    if (minimized_) {
        return kMinimizedTimeoutMs;
    }

    auto now = std::chrono::steady_clock::now();
    auto last_activity = std::max(last_user_input_, last_mcp_input_);
    auto elapsed = now - last_activity;

    if (elapsed >= idle_grace_) {
        // Idle visible window: same deep-sleep timeout as minimized.
        // File-watcher and SR-task polls run at ~2 Hz which is
        // adequate; user input, MCP requests, and window restores
        // all have immediate wake-up paths of their own.
        return kMinimizedTimeoutMs;
    }

    return kActiveTimeoutMs;
}

bool IdleTracker::should_render() const {
    if (minimized_) {
        return false;
    }

    auto now = std::chrono::steady_clock::now();
    auto last_activity = std::max(last_user_input_, last_mcp_input_);
    auto elapsed = now - last_activity;

    // Skip the render pass when idle -- nothing on screen needs
    // updating and there are no user / MCP requests to service.
    return elapsed < idle_grace_;
}

bool IdleTracker::is_idle() const {
    if (minimized_) return true;
    auto now = std::chrono::steady_clock::now();
    auto last_activity = std::max(last_user_input_, last_mcp_input_);
    return (now - last_activity) >= idle_grace_;
}

// ---------------------------------------------------------------------------
// classify_sdl_event
// ---------------------------------------------------------------------------

// Raw SDL event type constants.  We use numeric literals rather than
// the SDL header enums so this file compiles without <SDL_events.h>.
// These values are stable across SDL 2.x.

// Mouse
static constexpr std::uint32_t kSDLMouseMotion    = 0x400;
static constexpr std::uint32_t kSDLMouseButtonDown = 0x401;
static constexpr std::uint32_t kSDLMouseButtonUp   = 0x402;
static constexpr std::uint32_t kSDLMouseWheel      = 0x403;

// Keyboard
static constexpr std::uint32_t kSDLKeyDown  = 0x300;
static constexpr std::uint32_t kSDLKeyUp    = 0x301;

// Text
static constexpr std::uint32_t kSDLTextInput = 0x303;
static constexpr std::uint32_t kSDLTextEditing = 0x302;

// Drop
static constexpr std::uint32_t kSDLDropFile    = 0x1000;
static constexpr std::uint32_t kSDLDropText    = 0x1001;
static constexpr std::uint32_t kSDLDropBegin   = 0x1002;
static constexpr std::uint32_t kSDLDropComplete = 0x1003;

// Window
static constexpr std::uint32_t kSDLWindowEvent = 0x200;

// Touch
static constexpr std::uint32_t kSDLFingerDown  = 0x700;
static constexpr std::uint32_t kSDLFingerUp    = 0x701;
static constexpr std::uint32_t kSDLFingerMotion = 0x702;

// Gesture (also user input)
static constexpr std::uint32_t kSDLDollarGesture = 0x800;
static constexpr std::uint32_t kSDLDollarRecord  = 0x801; // NOLINT
static constexpr std::uint32_t kSDLMultiGesture   = 0x802;

IdleTracker::EventClass classify_sdl_event(std::uint32_t type) {
    switch (type) {
        // User input -- breaks idle.
        case kSDLMouseMotion:
        case kSDLMouseButtonDown:
        case kSDLMouseButtonUp:
        case kSDLMouseWheel:
        case kSDLKeyDown:
        case kSDLKeyUp:
        case kSDLTextInput:
        case kSDLTextEditing:
        case kSDLDropFile:
        case kSDLDropText:
        case kSDLDropBegin:
        case kSDLDropComplete:
        case kSDLFingerDown:
        case kSDLFingerUp:
        case kSDLFingerMotion:
        case kSDLDollarGesture:
        case kSDLDollarRecord:
        case kSDLMultiGesture:
            return IdleTracker::EventClass::UserInput;

        // Window events -- handled separately by set_minimized().
        case kSDLWindowEvent:
            return IdleTracker::EventClass::Window;

        default:
            return IdleTracker::EventClass::Other;
    }
}

} // namespace idiff
