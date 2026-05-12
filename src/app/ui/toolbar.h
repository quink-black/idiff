#ifndef IDIFF_APP_UI_TOOLBAR_H
#define IDIFF_APP_UI_TOOLBAR_H

#include "core/image_loader.h"    // LoaderBackend
#include "core/image_processor.h" // UpscaleMethod

#include <functional>

namespace idiff {

class Viewport;

// Inputs and outputs of the main menu / toolbar bar.  Pointers are
// non-owning views; the host owns lifetime.
struct ToolbarInputs {
    Viewport* viewport;

    // Persistent toggles managed by the host.  The toolbar mutates
    // them in place when the user toggles the corresponding menu
    // item.
    bool* show_image_list;
    bool* show_inspector;
    UpscaleMethod* upscale_method;

    // True when at least one image is loaded; gates the "Save
    // Viewport As..." menu item.
    bool any_entries_loaded;

    // Loader backend selector accessors.  Read returns the currently
    // active backend; write switches it (the host is responsible for
    // calling reload_all_images afterwards via on_reload_all_images).
    std::function<LoaderBackend()> get_loader_backend;
    std::function<void(LoaderBackend)> set_loader_backend;

    // Action callbacks.  All optional; missing callbacks make their
    // menu items inert.
    std::function<void()> on_open_files;
    std::function<void()> on_open_comparison_config;
    std::function<void()> on_save_viewport;
    std::function<void()> on_request_quit;
    std::function<void()> on_reload_all_images;

    // Invoked after the user changed channel view or background so
    // the host can mark the affected textures dirty.  May be empty.
    std::function<void()> on_view_invalidated;
};

void render_toolbar(const ToolbarInputs& in);

} // namespace idiff

#endif // IDIFF_APP_UI_TOOLBAR_H
