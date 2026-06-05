#ifndef IDIFF_APP_UI_IMAGE_LIST_H
#define IDIFF_APP_UI_IMAGE_LIST_H

#include <functional>
#include <vector>

namespace idiff {

class ComparisonConfigService;
class DiffService;
class SelectionModel;
class SrTaskService;
struct ImageEntry;

// Inputs to render_image_list.  Pointers are non-owning views; the
// host owns lifetime and must keep them alive across the call.
//
// Mutable fields (entries, selection, diff_service, show_image_list)
// are written by the renderer in response to user input:
//   * selection toggles flip the corresponding bit
//   * selection toggles also mark every selected entry's texture
//     dirty so the upscale-on-selection logic re-uploads it
//   * diff_service is marked dirty whenever the selection changes
//   * show_image_list is flipped when the user closes the panel via
//     the window's [x] button
struct ImageListInputs {
    std::vector<ImageEntry>* entries;
    SelectionModel* selection;
    DiffService* diff_service;
    const ComparisonConfigService* comparison_config;
    const SrTaskService* sr_service;

    // Panel close button writes through this pointer.
    bool* show_image_list;

    // True when an upscaler is detected at startup; gates the
    // "Super Resolution..." context-menu item.
    bool sr_enabled;

    // "Group by Name" toggle.  When true, clicking an entry selects
    // all entries sharing its group key (filename stem before the
    // last extension dot).  The checkbox in the panel writes through
    // this pointer.
    bool* group_by_name_ptr = nullptr;

    // Current "anchor" for Shift+click range selection.  -1 means no
    // anchor.  Owned by App::State; the renderer updates it on click.
    int* last_clicked_index = nullptr;

    // Returns the entries-index currently used as the reference image
    // for overlay / diff (-1 when the selection is empty).  Provided
    // as a callback so the list does not duplicate the selection logic.
    std::function<void(int& ref_idx)> get_ref_index;

    // True when the entry at the given index wraps a YuvRawSource
    // and therefore supports the "Edit YUV parameters..." menu
    // item.  The renderer cannot dynamic_cast directly because
    // MediaSource is not exposed in this header.
    std::function<bool(int entry_idx)> entry_is_yuv;

    // Action callbacks.  All optional; missing callbacks make the
    // associated UI inert.
    std::function<void()> on_open_files;
    std::function<void(int group_idx)> on_switch_comparison_group;
    std::function<void(int from, int to)> on_move_entry;
    std::function<void(int entry_idx)> on_mark_as_reference;
    std::function<void()> on_select_all;
    std::function<void(int entry_idx)> on_select_only_this;
    std::function<void(int entry_idx)> on_select_group;
    // Group-aware Selectable click handler.  When set, the renderer
    // calls this for plain clicks in group-by-name mode instead of
    // mutating the selection itself: clicking an entry of a foreign
    // group switches and selects the whole new group; clicking an
    // entry of the active group toggles just that single entry.
    std::function<void(int entry_idx)> on_click_in_group;
    std::function<void()> on_invert_selection;
    std::function<void()> on_unselect_all;
    std::function<void(int from, int to)> on_select_range;
    std::function<void(int entry_idx)> on_reload_entry;
    std::function<void()> on_reload_all;
    std::function<void(int entry_idx)> on_remove_entry;
    std::function<void()> on_remove_selected;
    std::function<void()> on_remove_all;
    std::function<void(int entry_idx)> on_edit_yuv_entry;
    std::function<void(int entry_idx)> on_open_sr_dialog;

    // Invoked after any persistent setting changed (group_by_name,
    // panel visibility) so the host can save settings.
    std::function<void()> on_settings_changed;
};

void render_image_list(const ImageListInputs& in);

} // namespace idiff

#endif // IDIFF_APP_UI_IMAGE_LIST_H
