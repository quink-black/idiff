#ifndef IDIFF_APP_UI_INSPECTOR_H
#define IDIFF_APP_UI_INSPECTOR_H

#include <functional>
#include <vector>

namespace idiff {

class MetricsPanel;
class PixelInspectorPanel;
class PropertiesPanel;
class SelectionModel;
class Viewport;
struct ImageEntry;

// Inputs to render_right_sidebar.  Pointers are non-owning views;
// the host owns lifetime.  The sidebar reads and writes:
//   * show_inspector (close button)
//   * entries[*].measurements (measurement deletions sync back here)
struct InspectorInputs {
    std::vector<ImageEntry>* entries;
    const SelectionModel* selection;
    Viewport* viewport;

    // Resolves the current reference entry index in the same way as
    // the image list and viewport.  May be -1 when the selection is
    // empty.
    std::function<void(int& ref_idx)> get_ref_index;

    // Maps each viewport slot index back to an entries index.  Populated
    // by render_viewport earlier in the same frame and consumed by the
    // Measurements tab to label slot rows.
    const std::vector<int>* viewport_slot_to_entry;

    // Optional inspector sub-panels.  nullptr-safe; missing panels skip
    // the corresponding tabs.
    PropertiesPanel* properties_panel;
    MetricsPanel* metrics_panel;
    // Optional pixel-inspector sub-panel.  Owned by the host (App)
    // because it carries per-session state (pinned samples) that
    // outlives a single render call.  nullptr-safe.
    PixelInspectorPanel* pixel_panel = nullptr;

    // Index of the currently-selected sub-panel.  Persisted into
    // AppSettings so the user does not have to reselect on every
    // launch.  Values map to: 0=Properties, 1=Pixel, 2=Metrics,
    // 3=Statistics, 4=Measurements.  Out-of-range values are clamped
    // back to 0 by the renderer.
    int* current_panel = nullptr;
    // Optional callback fired the moment the user picks a different
    // sub-panel via the Combo.  Used by the host to persist the new
    // value (e.g. AppSettings::save).  May be empty.
    std::function<void()> on_panel_changed;

    // Panel close button writes through this pointer.
    bool* show_inspector;
};

void render_right_sidebar(const InspectorInputs& in);

} // namespace idiff

#endif // IDIFF_APP_UI_INSPECTOR_H
