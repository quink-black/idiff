#ifndef IDIFF_DOMAIN_TIMELINE_MODEL_H
#define IDIFF_DOMAIN_TIMELINE_MODEL_H

// Timeline model.
//
// Owns the shared current-frame index that all multi-frame entries
// (e.g. raw YUV streams) follow when read_frame() is called against
// them.  Each entry carries its own per-entry frame_offset so two
// streams that start at different frames can be aligned without
// touching the shared index.
//
// The model is deliberately a thin value-with-helpers wrapper rather
// than a pub/sub source-of-truth: App keeps the state of pixel
// caches and ImGui widgets, and asks the model "what is the current
// frame?" / "decode every entry to the current frame".  This keeps
// the migration path obvious.
//
// Threading: not thread-safe; main thread only.

#include <chrono>
#include <string>
#include <vector>

namespace idiff {

struct ImageEntry;

class TimelineModel {
public:
    TimelineModel() = default;

    // Shared current frame index.  Not clamped against any entry's
    // length; callers should clamp before calling sync_to() when the
    // index might be outside [0, length(entries)).
    int current_frame() const noexcept { return current_frame_; }
    void set_current_frame(int v) noexcept { current_frame_ = v; }

    // Returns the maximum number of frames across every entry whose
    // source advertises more than one frame, clamped to at least 1.
    // Single-frame (or null-source) entries do not contribute.
    static int length(const std::vector<ImageEntry>& entries) noexcept;

    // Re-decode every multi-frame entry so its `image` cache holds
    // frame (current_frame + entry.frame_offset), pinning to the
    // entry's [0, count-1] when the offset would run past either end.
    // Entries whose source.read_frame() fails are left with the
    // previous pixels so the viewport doesn't go blank; the failing
    // filename and the source's last_error() are appended to
    // out_status (one " | "-separated line per failing entry).
    //
    // Returns true iff at least one entry's cached frame actually
    // changed (so the caller can mark textures and the diff cache
    // dirty).  The caller must clamp current_frame_ into [0,
    // length(entries)-1] before calling this; sync_to does not
    // adjust the shared index itself.
    bool sync_to(std::vector<ImageEntry>& entries, std::string& out_status);

    // Like sync_to but uses read_keyframe() for fast approximate
    // decoding.  Updates image and texture_dirty but does NOT update
    // cached_frame, so a subsequent sync_to() will still perform an
    // exact decode.  Returns true if any entry was updated.
    bool preview_to(std::vector<ImageEntry>& entries);

    // Convenience: clamp current_frame_ into [0, length(entries)-1].
    // Returns true if the value was actually changed.  Negative
    // lengths are coerced to length=1 -> current_frame_=0.
    bool clamp_to_length(const std::vector<ImageEntry>& entries) noexcept;

    // ---- Playback -------------------------------------------------------
    //
    // Auto-play advances current_frame_ at a fixed fps driven by a
    // steady-clock accumulator ticked once per App::frame().  At end
    // the index loops back to 0.  The caller (App) is responsible
    // for calling sync_entries_to_timeline() when tick_playback()
    // reports an advance.

    bool playing() const noexcept { return playing_; }
    // Resetting to play re-arms next_frame_time_ to now so playback
    // starts immediately rather than waiting for a stale deadline.
    void set_playing(bool v) noexcept {
        playing_ = v;
        if (v) next_frame_time_ = std::chrono::steady_clock::now();
    }
    double playback_fps() const noexcept { return playback_fps_; }
    // Clamp to [1, 120]; values outside that range are pinned.
    void set_playback_fps(double fps) noexcept;

    // Advance current_frame_ by one when the steady clock has passed
    // next_frame_time_, looping back to 0 at length-1.  Returns true
    // when a frame advance actually happened so the caller can call
    // sync_to().  Returns false when playing is false, the clock has
    // not yet reached the next frame time, or length(entries) <= 1.
    bool tick_playback(const std::vector<ImageEntry>& entries) noexcept;

private:
    int current_frame_ = 0;

    bool playing_ = false;
    double playback_fps_ = 30.0;
    std::chrono::steady_clock::time_point next_frame_time_{};
};

} // namespace idiff

#endif // IDIFF_DOMAIN_TIMELINE_MODEL_H
