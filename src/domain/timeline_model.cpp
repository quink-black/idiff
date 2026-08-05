#include "domain/timeline_model.h"

#include "app/app.h"
// Image and MediaSource are forward-declared in app.h via unique_ptr;
// instantiating reset()/destructors here requires the complete types.
#include "core/image.h"        // IWYU pragma: keep
#include "core/media_source.h" // IWYU pragma: keep
#include "util/logger.h"

#include <algorithm>
#include <string>

namespace idiff {

int TimelineModel::length(const std::vector<ImageEntry>& entries) noexcept {
    int max_frames = 1;
    for (const auto& e : entries) {
        if (e.source) {
            max_frames = std::max(max_frames, e.source->frame_count());
        }
    }
    return max_frames;
}

// Re-decode every multi-frame entry to the shared timeline index.
// Pixels are kept resident after decode so the user can scrub without
// re-decoding on every frame; AppController::touch_lazy is called by
// the controller after sync_to / preview_to returns, promoting each
// decoded entry to the front of the LRU so the next eviction sweep
// (fired on selection change) leaves scrub-active entries alone.
bool TimelineModel::sync_to(std::vector<ImageEntry>& entries,
                            std::string& out_status) {
    bool any_changed = false;
    for (auto& e : entries) {
        if (!e.source) continue;
        const int count = e.source->frame_count();
        if (count <= 1) continue;  // still image -- timeline doesn't apply

        int target = current_frame_ + e.frame_offset;
        if (target < 0) target = 0;
        if (target >= count) target = count - 1;

        if (target == e.cached_frame && e.image) continue;

        auto img = e.source->read_frame(target);
        if (!img) {
            std::string msg = "Frame read failed for " + e.filename
                            + " (" + e.source->last_error() + ")";
            LOG_WARN("%s", msg.c_str());
            if (!out_status.empty()) out_status += " | ";
            out_status += msg;
            continue;
        }
        e.image = std::move(img);
        e.display_image.reset();
        e.texture_dirty = true;
        e.cached_frame = target;
        any_changed = true;
    }
    return any_changed;
}

// Fast approximate preview for scrubbing: uses read_keyframe() so the
// decoder can skip to the nearest keyframe instead of decoding every
// intermediate frame.  Like sync_to, decoded pixels stay resident;
// AppController::touch_lazy keeps scrubbed entries in the LRU front.
// cached_frame is intentionally NOT updated so a subsequent sync_to()
// still performs an exact decode at this position.
bool TimelineModel::preview_to(std::vector<ImageEntry>& entries) {
    bool any_changed = false;
    for (auto& e : entries) {
        if (!e.source) continue;
        const int count = e.source->frame_count();
        if (count <= 1) continue;

        int target = current_frame_ + e.frame_offset;
        if (target < 0) target = 0;
        if (target >= count) target = count - 1;

        // Do not update cached_frame so a subsequent sync_to() will
        // still perform an exact decode at this position.
        auto img = e.source->read_keyframe(target);
        if (!img) continue;

        e.image = std::move(img);
        e.display_image.reset();
        e.texture_dirty = true;
        any_changed = true;
    }
    return any_changed;
}

bool TimelineModel::clamp_to_length(const std::vector<ImageEntry>& entries) noexcept {
    const int len = length(entries);
    int v = current_frame_;
    if (v < 0) v = 0;
    if (v >= len) v = len - 1;
    if (v < 0) v = 0;  // length is at least 1, but be defensive
    if (v == current_frame_) return false;
    current_frame_ = v;
    return true;
}

void TimelineModel::set_playback_fps(double fps) noexcept {
    if (fps < 1.0) fps = 1.0;
    if (fps > 120.0) fps = 120.0;
    playback_fps_ = fps;
}

bool TimelineModel::tick_playback(
    const std::vector<ImageEntry>& entries) noexcept {
    if (!playing_) return false;
    const int len = length(entries);
    if (len <= 1) return false;
    auto now = std::chrono::steady_clock::now();
    if (now < next_frame_time_) return false;
    int next = current_frame_ + 1;
    if (next >= len) next = 0;  // loop
    current_frame_ = next;
    using double_sec = std::chrono::duration<double>;
    auto interval = std::chrono::duration_cast<
        std::chrono::steady_clock::duration>(double_sec(1.0 / playback_fps_));
    next_frame_time_ = now + interval;
    return true;
}

} // namespace idiff
