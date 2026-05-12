#ifndef IDIFF_APP_UI_INSPECTOR_H
#define IDIFF_APP_UI_INSPECTOR_H

#include <functional>
#include <vector>

namespace idiff {

class MetricsPanel;
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

    // Resolves the current A and B entries-indices in the same swap-aware
    // way as the image list and viewport.  Either may be -1 when the
    // selection is empty or holds a single entry.
    std::function<void(int& a_idx, int& b_idx)> get_ab_indices;

    // Maps each viewport slot index back to an entries index.  Populated
    // by render_viewport earlier in the same frame and consumed by the
    // Measurements tab to label slot rows.
    const std::vector<int>* viewport_slot_to_entry;

    // Optional inspector sub-panels.  nullptr-safe; missing panels skip
    // the corresponding tabs.
    PropertiesPanel* properties_panel;
    MetricsPanel* metrics_panel;

    // Panel close button writes through this pointer.
    bool* show_inspector;
};

void render_right_sidebar(const InspectorInputs& in);

} // namespace idiff

#endif // IDIFF_APP_UI_INSPECTOR_H
