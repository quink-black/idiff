#ifndef IDIFF_APP_UI_STATUS_BAR_H
#define IDIFF_APP_UI_STATUS_BAR_H

#include <functional>
#include <string>
#include <vector>

namespace idiff {

class Viewport;
class DiffService;
class SrTaskService;
class TimelineModel;
class SelectionModel;
struct ImageEntry;

// Inputs to render_status_bar.  Bundled into a single struct so the
// renderer signature stays stable as the App grows; everything is a
// non-owning view that must outlive the call.
struct StatusBarInputs {
    const std::vector<ImageEntry>* entries;
    const SelectionModel* selection;
    const Viewport* viewport;
    const DiffService* diff_service;
    const SrTaskService* sr_service;

    // Maps a viewport slot index back to an entries index.  Populated
    // by the viewport renderer earlier in the same frame and consumed
    // here to translate a hovered cell into "which image is under the
    // cursor".
    const std::vector<int>* viewport_slot_to_entry;

    // Most recent persistent and SR-notification status text.
    const std::string* status_text;
    const std::string* status_msg;

    // Returns the entries-index currently used as the reference image
    // for overlay / diff (-1 when the selection is empty).  Provided
    // as a callback so the renderer does not duplicate the selection
    // logic.
    std::function<void(int& ref_idx)> get_ref_index;

    // Per-instance identity ("idiff:<pid>") and the matching socket
    // path.  The status bar shows the identity as a small chip on the
    // left so the user can correlate this window with a /tmp/idiff-*
    // .sock and with whatever the MCP / RPC client targeted.  Both
    // are empty when the RPC server is disabled or failed to start;
    // the chip is omitted in that case.
    const std::string* identity_label = nullptr;
    const std::string* identity_socket_path = nullptr;
};

void render_status_bar(const StatusBarInputs& in);

// Inputs and side-effect outputs of the timeline scrub bar.
struct TimelineBarInputs {
    std::vector<ImageEntry>* entries;  // mutated: per-entry frame_offset edits
    TimelineModel* timeline;           // mutated: clamp + current_frame edits

    // Invoked after a frame index or per-entry offset changed so the
    // host can re-decode every multi-frame source.
    std::function<void()> on_frame_changed;

    // Invoked during slider drag for fast keyframe preview.
    // on_frame_changed is called on release for the exact decode.
    std::function<void()> on_frame_preview;
};

// Render the timeline scrub bar above the status bar.  Returns the
// bar height in pixels (0 when the bar was not drawn because no
// multi-frame source is loaded) so the caller can shrink the docking
// area by that amount.
float render_timeline_bar(const TimelineBarInputs& in);

} // namespace idiff

#endif // IDIFF_APP_UI_STATUS_BAR_H
