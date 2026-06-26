#include <catch2/catch_test_macros.hpp>

#include "app/idle_policy.h"

#include <SDL_video.h>

#include <chrono>
#include <thread>

using namespace idiff;

// These tests pin the idle-CPU contract.  They exist because the
// regression they guard against (idiff burning ~8% CPU while idle or
// minimized) is invisible in a functional smoke test: the app still
// "works", it just wastes power.  Asserting the policy directly catches
// a revert to a busy-spin timeout or to rendering a hidden window.

TEST_CASE("idle policy: wait timeout never busy-spins") {
    // A zero timeout would turn SDL_WaitEventTimeout back into a
    // non-blocking poll and reintroduce the spin.
    CHECK(loop_wait_timeout_ms(false) > 0);
    CHECK(loop_wait_timeout_ms(true) > 0);
}

TEST_CASE("idle policy: minimized window idles at least as deep as visible") {
    CHECK(loop_wait_timeout_ms(true) >= loop_wait_timeout_ms(false));
}

TEST_CASE("idle policy: rendering is skipped only while minimized") {
    CHECK(loop_should_render(false));
    CHECK_FALSE(loop_should_render(true));
}

TEST_CASE("idle policy: minimize and hide enter low-power state") {
    CHECK(apply_window_event(false, SDL_WINDOWEVENT_MINIMIZED));
    CHECK(apply_window_event(false, SDL_WINDOWEVENT_HIDDEN));
    // Idempotent: already-minimized stays minimized.
    CHECK(apply_window_event(true, SDL_WINDOWEVENT_MINIMIZED));
}

TEST_CASE("idle policy: restore and show leave low-power state") {
    CHECK_FALSE(apply_window_event(true, SDL_WINDOWEVENT_RESTORED));
    CHECK_FALSE(apply_window_event(true, SDL_WINDOWEVENT_SHOWN));
    // Idempotent: already-visible stays visible.
    CHECK_FALSE(apply_window_event(false, SDL_WINDOWEVENT_RESTORED));
}

TEST_CASE("idle policy: unrelated window events leave state unchanged") {
    // Losing focus must not be treated as minimize -- a visible but
    // unfocused window should keep rendering.
    CHECK_FALSE(apply_window_event(false, SDL_WINDOWEVENT_FOCUS_LOST));
    CHECK(apply_window_event(true, SDL_WINDOWEVENT_FOCUS_GAINED));
    CHECK_FALSE(apply_window_event(false, SDL_WINDOWEVENT_MOVED));
    CHECK(apply_window_event(true, SDL_WINDOWEVENT_EXPOSED));
}

// ---------------------------------------------------------------------------
// IdleTracker tests
// ---------------------------------------------------------------------------

TEST_CASE("IdleTracker: starts in active state, renders every frame") {
    IdleTracker t;
    CHECK_FALSE(t.is_minimized());
    CHECK(t.should_render());
    CHECK_FALSE(t.is_idle());
    CHECK(t.wait_timeout_ms() == 16);
}

TEST_CASE("IdleTracker: render is skipped after idle grace expires") {
    // Use a very short grace so the test does not have to sleep.
    IdleTracker t{std::chrono::milliseconds(10)};
    CHECK(t.should_render());

    // Wait for the grace period to elapse.
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK_FALSE(t.should_render());
    CHECK(t.is_idle());
    CHECK(t.wait_timeout_ms() == 200);
}

TEST_CASE("IdleTracker: user input resets idle timer and resumes render") {
    IdleTracker t{std::chrono::milliseconds(10)};

    // Go idle.
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK_FALSE(t.should_render());

    // Simulate a mouse click.
    t.on_user_input();
    CHECK(t.should_render());
    CHECK_FALSE(t.is_idle());
    CHECK(t.wait_timeout_ms() == 16);
}

TEST_CASE("IdleTracker: MCP activity resets idle timer and resumes render") {
    IdleTracker t{std::chrono::milliseconds(10)};

    // Go idle.
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK_FALSE(t.should_render());

    // Simulate an RPC request being dispatched.
    t.on_mcp_activity();
    CHECK(t.should_render());
    CHECK_FALSE(t.is_idle());
}

TEST_CASE("IdleTracker: minimized always skips render regardless of idle") {
    IdleTracker t;
    t.set_minimized(true);
    CHECK(t.is_minimized());
    CHECK_FALSE(t.should_render());
    CHECK(t.is_idle());
    CHECK(t.wait_timeout_ms() == 100);
}

TEST_CASE("IdleTracker: restore from minimized counts as user input") {
    IdleTracker t{std::chrono::milliseconds(10)};

    // Go idle first.
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK_FALSE(t.should_render());

    // Then minimize and restore -- restore should re-activate.
    t.set_minimized(true);
    t.set_minimized(false);
    CHECK(t.should_render());
    CHECK_FALSE(t.is_idle());
}

TEST_CASE("IdleTracker: on_sdl_event classifies UserInput") {
    IdleTracker t{std::chrono::milliseconds(10)};

    // Go idle.
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK_FALSE(t.should_render());

    // Simulate a key-down event (0x300).
    auto cls = t.on_sdl_event(0x300);
    CHECK(cls == IdleTracker::EventClass::UserInput);
    CHECK(t.should_render());
}

TEST_CASE("IdleTracker: on_sdl_event classifies Window events") {
    IdleTracker t;
    auto cls = t.on_sdl_event(0x200, SDL_WINDOWEVENT_MINIMIZED);
    CHECK(cls == IdleTracker::EventClass::Window);
}

TEST_CASE("IdleTracker: on_sdl_event classifies Other events") {
    IdleTracker t{std::chrono::milliseconds(10)};

    // Go idle.
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK_FALSE(t.should_render());

    // A timer event (0x5000 range) should not break idle.
    auto cls = t.on_sdl_event(0x5000);
    CHECK(cls == IdleTracker::EventClass::Other);
    CHECK_FALSE(t.should_render());
}

TEST_CASE("classify_sdl_event: mouse events are UserInput") {
    CHECK(classify_sdl_event(0x400) == IdleTracker::EventClass::UserInput);  // motion
    CHECK(classify_sdl_event(0x401) == IdleTracker::EventClass::UserInput);  // down
    CHECK(classify_sdl_event(0x402) == IdleTracker::EventClass::UserInput);  // up
    CHECK(classify_sdl_event(0x403) == IdleTracker::EventClass::UserInput);  // wheel
}

TEST_CASE("classify_sdl_event: keyboard events are UserInput") {
    CHECK(classify_sdl_event(0x300) == IdleTracker::EventClass::UserInput);  // down
    CHECK(classify_sdl_event(0x301) == IdleTracker::EventClass::UserInput);  // up
}

TEST_CASE("classify_sdl_event: text and drop events are UserInput") {
    CHECK(classify_sdl_event(0x303) == IdleTracker::EventClass::UserInput);  // text
    CHECK(classify_sdl_event(0x1000) == IdleTracker::EventClass::UserInput); // drop
}

TEST_CASE("classify_sdl_event: window events are Window") {
    CHECK(classify_sdl_event(0x200) == IdleTracker::EventClass::Window);
}

TEST_CASE("classify_sdl_event: unknown events are Other") {
    CHECK(classify_sdl_event(0x9999) == IdleTracker::EventClass::Other);
    CHECK(classify_sdl_event(0) == IdleTracker::EventClass::Other);
}
