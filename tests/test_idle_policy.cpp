#include <catch2/catch_test_macros.hpp>

#include "app/idle_policy.h"

#include <SDL_video.h>

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
