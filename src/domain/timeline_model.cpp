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

} // namespace idiff
