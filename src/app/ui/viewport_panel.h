#ifndef IDIFF_APP_UI_VIEWPORT_PANEL_H
#define IDIFF_APP_UI_VIEWPORT_PANEL_H

#include "core/image_comparator.h" // HeatmapColor
#include "app/viewport.h"          // ChannelViewMode

#include <functional>
#include <string>
#include <vector>

namespace idiff {

class DiffService;
class SelectionModel;
class Viewport;
struct AppSettings;
struct ImageEntry;

// Inputs to render_viewport_panel.  Pointers are non-owning views; the
// host owns lifetime and must keep them alive for the duration of the
// call.
//
// Mutable fields written by the renderer:
//   * entries[*].texture_dirty (after a channel-view change)
//   * entries[*].measurements (when a measurement is committed,
//     and when the user clears or deletes a measurement)
//   * diff_service (mark_dirty on heatmap / amplification edits)
//   * viewport (input gestures, render call)
//   * settings (persisted on every toolbar option change)
//   * status_text (passed to DiffService::update for failure reporting)
//   * diff_amplification, heatmap_color (mirror to settings)
//   * viewport_slot_to_entry (rebuilt from scratch every frame)
//   * last_channel_view_mode (change-detection latch)
//   * sel_drag_is_ctrl (cross-frame drag-source latch)
struct ViewportPanelInputs {
    std::vector<ImageEntry>* entries;
    SelectionModel* selection;
    Viewport* viewport;
    DiffService* diff_service;
    AppSettings* settings;

    std::string* status_text;
    double* diff_amplification;
    HeatmapColor* heatmap_color;

    std::vector<int>* viewport_slot_to_entry;
    ChannelViewMode* last_channel_view_mode;
    bool* sel_drag_is_ctrl;

    // Resolves the current reference entry index in the same way as
    // the image list and inspector.
    std::function<void(int& ref_idx)> get_ref_index;

    // Re-runs the channel-view + upscale pipeline for the entry at the
    // given index and uploads the resulting pixels to its SDL texture.
    // Called once per dirty selected entry at the top of the frame.
    std::function<void(int entry_idx)> on_update_display_image;
    std::function<void(int entry_idx)> on_upload_texture;

    // Save Viewport... toolbar button.  May be empty.
    std::function<void()> on_save_viewport;

    // Invoked after any persistent setting changed so the host can
    // synchronize state and save.  May be empty.
    std::function<void()> on_settings_changed;

    // Fires once per Shift+left-click over the viewport when there is a
    // valid hover sample.  Used to forward the cursor position to the
    // pixel inspector as a "quick pin".  Coexists with pan / measure /
    // selection-zoom because Shift is otherwise unused by the existing
    // gestures; nullptr-safe.
    std::function<void()> on_shift_pin_click;

    // Called once per frame when the selection just changed.  The host
    // uses this to release decoded pixel data for entries that are no
    // longer selected, reducing memory usage.  May be empty.
    std::function<void()> on_evict_non_selected;
};

void render_viewport_panel(const ViewportPanelInputs& in);

} // namespace idiff

#endif // IDIFF_APP_UI_VIEWPORT_PANEL_H
