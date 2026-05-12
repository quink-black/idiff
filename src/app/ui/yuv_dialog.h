#ifndef IDIFF_APP_UI_YUV_DIALOG_H
#define IDIFF_APP_UI_YUV_DIALOG_H

#include "core/media_source.h"

#include <functional>
#include <string>
#include <vector>

namespace idiff {

// All UI-side state of the YUV-parameters modal.  The dialog has two
// modes:
//   * Load mode: pending_paths is non-empty.  Each call processes the
//     front path; on confirm/skip the entry is dequeued and the dialog
//     re-opens for the next one if any.
//   * Edit mode: editing_entry_idx >= 0.  pending_paths is ignored;
//     the dialog targets the entry the caller has already armed via
//     prepare_for_edit().  Cancel returns to idle without modifying
//     the entry.
//
// At most one mode may be active at a time.  The "needs_open" flag
// drives ImGui::OpenPopup on the next render and is cleared by the
// renderer after firing.
struct YuvDialogState {
    std::vector<std::string> pending_paths;
    YuvStreamParams params{};
    bool needs_open = false;
    int editing_entry_idx = -1;
};

// Callback bundle the YUV dialog needs from the host.  Kept narrow so
// the renderer never reaches into App or its services directly.
//
//   * resolve_entry_path: return the on-disk path for entry index
//     `idx` (used in edit mode to display the file size and frame
//     count preview).  Return an empty string when the index is out
//     of range; the dialog will silently abort the edit.
//   * default_load_params: starting parameters for load-mode priming
//     before guess_yuv_params_from_filename() refines them.
//   * on_load_confirm: user confirmed Load mode for `path` with
//     `params`.  Should add a new entry and persist defaults.
//   * on_edit_apply: user confirmed Edit mode for entry `idx` with
//     `params`.  Should rebuild the entry's source.
struct YuvDialogCallbacks {
    std::function<std::string(int idx)> resolve_entry_path;
    std::function<YuvStreamParams()> default_load_params;
    std::function<void(const std::string& path, const YuvStreamParams& params)>
        on_load_confirm;
    std::function<void(int idx, const YuvStreamParams& params)>
        on_edit_apply;
};

// Render the YUV-parameters modal.  No-op when both modes are idle
// (pending_paths empty AND editing_entry_idx < 0).  Mutates `state`
// in response to user actions and invokes the callbacks supplied by
// the host.
void render_yuv_params_dialog(YuvDialogState& state,
                              const YuvDialogCallbacks& callbacks);

} // namespace idiff

#endif // IDIFF_APP_UI_YUV_DIALOG_H
