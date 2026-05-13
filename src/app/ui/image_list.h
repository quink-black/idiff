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
//   * selection toggles flip the corresponding bit and reset swap_ab
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

    // Returns the entries-index pair currently used as A and B for
    // overlay / diff (-1 when missing).  Provided as a callback so
    // the list does not duplicate the swap-aware selection logic.
    std::function<void(int& a_idx, int& b_idx)> get_ab_indices;

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
    std::function<void()> on_select_all;
    std::function<void(int entry_idx)> on_select_only_this;
    std::function<void()> on_invert_selection;
    std::function<void()> on_unselect_all;
    std::function<void(int entry_idx)> on_remove_entry;
    std::function<void()> on_remove_selected;
    std::function<void()> on_remove_all;
    std::function<void(int entry_idx)> on_edit_yuv_entry;
    std::function<void(int entry_idx)> on_open_sr_dialog;
};

void render_image_list(const ImageListInputs& in);

} // namespace idiff

#endif // IDIFF_APP_UI_IMAGE_LIST_H
