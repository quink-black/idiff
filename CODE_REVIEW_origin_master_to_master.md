# Code Review: `origin/master..master`

Reviewed on 2026-04-18. 9 commits, all authored by Zhao Zhili, span 12:29 to 14:46 on the same day.

```
3b2c9f3 Keep ruler labels visible when zoomed by clamping to visible pixel range
ae2bb00 Anchor ruler strips to cell edges instead of image edges
074da80 Add ruler and grid overlay with toggle controls in viewport
7d8a291 Show per-image statistics for all selected images with lazy cache
35e9a2e Add per-channel min/max to single image statistics
f68481b Add RGB histogram display in the statistics panel
6474f53 Add per-channel RGB statistics for single images
8b1be5e Allow left-click pan in overlay mode by narrowing slider hit area
7101245 Add left-click drag to pan viewport and hint for right-click zoom-to-selection
```

---

## 1. Process / Commit-Hygiene Issues

### 1.1 Fix-up sequences violate G2 ("One Commit, One Purpose — no fix-ups on dev branch")

Two clear fix-up sequences exist and should have been squashed before landing on `master`:

| Base commit | Fix-up(s) | Problem |
|---|---|---|
| `7101245` Add left-click drag to pan viewport | `8b1be5e` Allow left-click pan in overlay mode by narrowing slider hit area | `7101245` introduced left-click pan but left overlay mode broken (slider swallowed all clicks). `8b1be5e` is a direct follow-up fix 13 minutes later. Per G2, `7101245` should not have been committed in a broken state; the two should be one atomic commit. |
| `074da80` Add ruler and grid overlay | `ae2bb00` Anchor ruler strips to cell edges instead of image edges, `3b2c9f3` Keep ruler labels visible when zoomed by clamping | Three consecutive commits on the same feature, the last two explicitly fix rendering bugs introduced by the first. This is the textbook case G2 warns against. |

**Evidence:** `ae2bb00`'s diff is a near-total rewrite of `draw_ruler` (cell-anchored strip, new signature with `cell_origin`/`cell_size`) only ~24 minutes after `074da80` introduced the image-anchored version. `3b2c9f3` then adds clamping that should have been part of the original design.

**Recommendation:** Either rebase/squash locally before pushing, or use the temp-branch workflow (`temp/feature-x` for iteration → clean `dev/feature-x` with atomic commits).

### 1.2 Commit `074da80` still mixes two purposes (ruler + grid)

Ruler and grid are two independent overlays with their own toggles, state, and draw functions. They could and should have been two separate commits. Mixing them also inflates the diff (+227 / −51 lines) and makes bisecting harder.

### 1.3 Drive-by formatting noise in `074da80`

`074da80` deletes ~16 explanatory comments from `viewport.cpp` that have nothing to do with the ruler/grid feature (e.g. the `// cx/cy = center of the cell + current pan offset.` block in `zoom_around`, `// Clip to cell`, `// Center in viewport, then apply pan`, etc.). This violates G1 ("never mix unrelated changes") and actively reduces code readability.

### 1.4 Commit message for `ae2bb00` is misleading

`ae2bb00`'s message — *"Anchor ruler strips to cell edges instead of image edges"* — understates the change. The commit also:

- Changes `draw_ruler`'s signature (breaking the API for any other caller).
- Updates **every** call site in `render_split`, `render_overlay`, `render_difference`.
- Tweaks the background alpha (200 → 220).
- Removes the extra `PushClipRect`/`PopClipRect` wrapping in split mode.

A reader scanning the log will not realize this from the title.

---

## 2. Correctness Bugs

### 2.1 `8b1be5e` — dead variable in `render_overlay`

The function takes `vp_origin_` and `vp_size_` as members. Inside the slider-interaction block:

```cpp
ImVec2 strip_pos(vp_origin_.x + std::max(0.0f, line_x - slider_hit_radius - vp_origin_.x),
                 vp_origin_.y);
float strip_x_local = line_x - vp_origin_.x - slider_hit_radius;
if (strip_x_local < 0) strip_x_local = 0;
```

`strip_pos` is **computed but never used** — the actual `SetCursorScreenPos` call uses `strip_x_local` directly. Dead code; drop it.

### 2.2 `8b1be5e` / overlay — redundant local shadowing of `line_x`

`line_x` is already declared earlier in `render_overlay` at the top of the function (used to draw the slider line and clip regions). The new slider-hit-test block re-declares it:

```cpp
float line_x = vp_origin_.x + vp_size_.x * slider_pos_;  // shadows outer line_x
```

Harmless but confusing; the outer one is already in scope. Reuse it.

### 2.3 `7101245` — `renderer_dragging` race with ImGui's drag state

In `app.cpp` the pan handler guards against the overlay slider but does **not** guard against right-drag selection (which starts via `ImGui::IsMouseClicked(ImGuiMouseButton_Right)`). They don't conflict today because they use different buttons, but the code path unconditionally pans on **any** `IsMouseDragging(Left)` over the viewport. Consider also gating on `!vp.selecting()` so future left-button modal interactions don't regress pan.

### 2.4 `074da80` — dead `|| true` expression

```cpp
// Label
if (px > 0 || true) {
    char buf[32];
    ...
}
```

The `|| true` makes the condition always `true`. Cleaned up in `ae2bb00`, but still a red flag that the original commit was written hastily and should not have shipped as-is. (Reinforces §1.1.)

### 2.5 `074da80` — integer-division in ruler step calculation

```cpp
int minor = std::max(1, interval / 5);
```

For `interval = 2`, `minor = 1` (ok). For `interval = 1`, `minor = 1`, meaning every minor tick equals every major tick and we enter the `px % interval == 0 ? continue` branch for all of them → no minor ticks drawn. Not a bug per se, but the logic should explicitly skip the minor-tick loop when `interval == 1`.

### 2.6 `7d8a291` — `stats_cache_` keyed by `display_label` is fragile

`render_statistics` keys the cache by the entry's `display_label`. This has two problems:

1. **Collisions**: two entries can share the same display label (common for config-driven loads — prefix `[A]` and `[B]` distinguish top-two slots but further selections reuse bare labels).
2. **Stale entries**: `invalidate_cache()` is only called on **selection checkbox** toggles. It is **not** called when:
   - The underlying `display_image` changes (YUV frame-step, decoder-param change, upscale re-run).
   - An entry is removed or the file is reloaded.
   - A/B swap (swap_ab_ toggles) — labels `[A]`/`[B]` now attach to different images but the cache is keyed by the old `[A] foo.png` string.

**Recommendation:** Key the cache by a stable identity (e.g. `Image*` pointer + version counter, or `entry.id`). Also invalidate on swap and on `display_image` replacement.

### 2.7 `7d8a291` — cache grows unbounded

`stats_cache_` is a `std::map<std::string, PerImageStats>` with no eviction. Each `PerImageStats` holds a full 3×256 `Histogram`. Over a long session with many ad-hoc selections, stale entries accumulate. The current `invalidate_cache()` clears **all** entries on any selection change, which is both too aggressive (invalidates valid data) and insufficient (doesn't trigger on image mutation).

### 2.8 `35e9a2e` — split/meanStdDev redundancy

In `compute_single`, `cv::meanStdDev` is called first, then `cv::split` + per-channel `cv::minMaxLoc`. The `cv::split` materialises 3 full-resolution `Mat`s just for min/max, when `cv::minMaxLoc` could be called on the multi-channel original with a mask, or better, a single pass over the data could compute mean/var/min/max together. For large images (4K+), this doubles the memory bandwidth.

### 2.9 `6474f53` — variance vs population variance unclear

`compute_single` reports `stddev * stddev` as "var". OpenCV's `cv::meanStdDev` returns the **population** stddev (divides by N, not N−1). The UI labels the column "Var" without qualification. Either (a) document this in the header, or (b) use the sample variance (N−1). For image analysis, population is conventional, but it should be stated.

### 2.10 `074da80` / `3b2c9f3` — ruler uses `std::snprintf` with no locale guard

`std::snprintf(buf, sizeof(buf), "%d", px)` is locale-sensitive on some platforms (thousands separator). Not a correctness issue today (C locale default), but worth noting for the `"%d"` case it's fine; the concern would arise if `%g`/`%f` were ever added. Low priority.

---

## 3. Design / Architecture Concerns

### 3.1 `MetricsPanel` has two overlapping rendering entry points

After `7d8a291`, the class now has:

- `render(a, b)` — legacy, appears unused?
- `render_inline(a, b)` — A/B metrics (used in Metrics tab)
- `render_statistics(images)` — per-image stats (used in Statistics tab)

Grep shows no callers of `render`. Dead code should be removed, or at minimum clearly commented.

### 3.2 `MetricsEngine` constructed inside render loops

Both `render_inline` and `render_image_stats` construct a fresh `MetricsEngine` on every "Compute" button click:

```cpp
MetricsEngine engine;
auto result = engine.compute_single(*image);
```

`MetricsEngine` is cheap to construct today, but this forces future engine additions (e.g. cached plans, persistent buffers) into global state. Prefer a member or dependency-injected instance.

### 3.3 `Viewport` `slider_dragging_` reset is fragile

`render()` resets `slider_dragging_ = false` at the top of every frame, and only `render_overlay` sets it to `true`. `App::render_viewport` reads it **after** calling `vp.render()`. Ordering is correct today, but the API contract ("valid until next render call") is buried in a terse header comment. Consider a clearer name (`last_frame_slider_dragging_`) or returning the state from `render_overlay`.

### 3.4 Ruler API couples "cell" and "image" positioning

`draw_ruler(img_pos, img_size, img_w, img_h, scale, cell_origin, cell_size)` has 7 parameters, 2 of which (`img_size`) are redundant (derivable from `img_w * scale`, `img_h * scale`). The signature invites errors at call sites. A `RulerLayout` struct would be easier to extend (e.g. when adding pan-clamp in `3b2c9f3`, the callers didn't need to change — but the function internals now rely on `cell_*` and `img_*` being consistent, which isn't enforced).

### 3.5 Grid/ruler toggles have no persistence

`show_ruler_` / `show_grid_` default to `false` every app launch. Most comparable tools (Photoshop, Figma, etc.) persist these preferences. If `AppConfig` already persists settings, these should join.

---

## 4. Testing

### 4.1 Positive: `35e9a2e` adds tests for `compute_single`

Good. Three test cases cover the happy path (constant image), per-channel distinguishing (corner pixels), and empty-image error path.

### 4.2 Missing: no tests for UI-layer changes

None of the viewport / metrics_panel UI changes (pan, slider hit-radius, ruler, grid, statistics panel) have tests. Understandable for pure ImGui code, but the testable parts have been missed:

- `compute_nice_interval(scale, min_spacing)` is a pure function — trivially testable.
- The cache-invalidation logic in `MetricsPanel` is testable without a renderer.

### 4.3 Missing: `7d8a291` regressions

The rename from `render_single` to `render_statistics` changes the call signature (now takes a vector of pairs). No compile-time check guarantees all callers were updated, and there are no tests pinning the behaviour. The old `render_single` is effectively deleted without deprecation.

---

## 5. Naming / Readability Nits

- `6474f53` uses `mean_r_`, `var_r_`, etc. as direct members on `MetricsPanel`. When `7d8a291` moved them into `PerImageStats`, these orphaned fields weren't removed from the header — actually they **were** removed in `7d8a291`. ✅
- `074da80` introduces `compute_nice_interval` as a private static on `Viewport`. It's a pure utility with no `Viewport` dependency; it could live in an anonymous namespace in the `.cpp`.
- `074da80` uses magic numbers liberally: `ruler_thickness = 18.0f`, `min_tick_spacing = 50.0f`, `bar_w = canvas_w / 256.0f`, `canvas_h = 100.0f` / later `80.0f`. Consider a `constexpr` block at file top.
- Hist color constants `IM_COL32(255, 80, 80, 160)` etc. are duplicated in `f68481b` and `7d8a291`. Extract.

---

## 6. Summary Table

| # | Severity | Category | Commit(s) | Issue |
|---|---|---|---|---|
| 1.1 | **High** | Process (G2) | `7101245+8b1be5e`, `074da80+ae2bb00+3b2c9f3` | Fix-up chains should be squashed |
| 1.2 | Medium | Process (G2) | `074da80` | Ruler and grid mixed in one commit |
| 1.3 | Medium | Process (G1) | `074da80` | Unrelated comment deletions |
| 1.4 | Low | Process (G4) | `ae2bb00` | Misleading commit message |
| 2.1 | Low | Correctness | `8b1be5e` | Dead `strip_pos` variable |
| 2.2 | Low | Readability | `8b1be5e` | Shadowed `line_x` |
| 2.3 | Low | Correctness | `7101245` | Pan should also gate on `selecting()` |
| 2.4 | Low | Correctness | `074da80` | `if (px > 0 \|\| true)` |
| 2.5 | Low | Correctness | `074da80` | Minor-tick logic for `interval == 1` |
| 2.6 | **High** | Correctness | `7d8a291` | Cache keyed by label, not invalidated on image mutation / A-B swap |
| 2.7 | Medium | Resource use | `7d8a291` | Unbounded cache growth |
| 2.8 | Medium | Performance | `35e9a2e` | Redundant `cv::split` + `meanStdDev` |
| 2.9 | Low | Docs | `6474f53` | Population vs sample variance unlabelled |
| 3.1 | Low | Design | `7d8a291` | Unused `render(a, b)` leftover |
| 3.2 | Low | Design | multiple | `MetricsEngine` re-constructed per click |
| 3.3 | Low | Design | `7101245` | `slider_dragging_` ordering contract is implicit |
| 3.4 | Low | Design | `ae2bb00` | `draw_ruler` takes 7 params, 2 redundant |
| 3.5 | Low | UX | `074da80` | Ruler/grid toggles not persisted |
| 4.1 | ✅ | Testing | `35e9a2e` | Tests added for `compute_single` — good |
| 4.2 | Medium | Testing | `074da80` | `compute_nice_interval` is pure & untested |

---

## 7. Recommended Actions (ordered by ROI)

1. **Fix cache invalidation in `MetricsPanel`** (§2.6, §2.7) — this is a live correctness bug users will hit when swapping A/B or stepping YUV frames.
2. **Adopt squashing before push**, or temp-branch workflow, to eliminate the `074da80+ae2bb00+3b2c9f3` style fix-up chains (§1.1).
3. **Split future commits by concern**: ruler vs grid, UI hints vs interaction changes (§1.2, §1.4).
4. Remove dead code (`strip_pos`, `|| true`, unused `render(a, b)`) — one cleanup commit (§2.1, §2.4, §3.1).
5. Add a unit test for `compute_nice_interval` (§4.2).
6. Persist `show_ruler_` / `show_grid_` in `AppConfig` (§3.5).

