#ifndef IDIFF_PROPERTIES_PANEL_H
#define IDIFF_PROPERTIES_PANEL_H

#include <vector>

namespace idiff {

class Image;
class Viewport;

class PropertiesPanel {
public:
    PropertiesPanel();
    ~PropertiesPanel();

    void render(const Image* image_a, const Image* image_b,
                const Image* display_a = nullptr, const Image* display_b = nullptr,
                const char* name_a = nullptr, const char* name_b = nullptr);
    void render_inline(const Image* image_a, const Image* image_b,
                       const Image* display_a = nullptr, const Image* display_b = nullptr,
                       const char* name_a = nullptr, const char* name_b = nullptr);

    // Render the "Measurements" list for the given viewport.  Each row
    // describes one saved rectangle using the source image's native
    // resolution: id, W x H px, source image label, and a trailing
    // delete button.  `slot_labels` is indexed by the measurement's
    // source_cell_index; out-of-range indices fall back to "cell N".
    // On return, `out_deleted_ids` contains the ids of measurements the
    // user asked to remove (via per-row x or "Clear All"); the caller
    // is responsible for syncing the deletion to the owning entries.
    void render_measurements(Viewport& viewport,
                              const std::vector<const char*>& slot_labels,
                              std::vector<int>& out_deleted_ids,
                              bool& out_clear_all);

private:
    void render_image_props(const char* slot_label, const char* name,
                            const Image* img, const Image* display_img);
};

} // namespace idiff

#endif // IDIFF_PROPERTIES_PANEL_H
