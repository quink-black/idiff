#ifndef IDIFF_APP_SETTINGS_H
#define IDIFF_APP_SETTINGS_H

#include <string>

#include "core/media_source.h"

namespace idiff {

// Persistent settings shared across runs of idiff.  Currently only
// tracks "the last YUV parameters the user confirmed" so that the
// next time they open a .yuv file without recognizable filename
// hints, the dialog is prefilled with a sensible default.
//
// The file is a simple UTF-8 key=value text file.  We avoid a JSON
// dependency for such a small amount of state; if the schema grows,
// migrate to a real serializer.
struct AppSettings {
    YuvStreamParams last_yuv_params{};

    // Viewport overlay toggles.  Persisted so the user does not have
    // to re-enable them every launch.
    bool show_ruler = false;
    bool show_grid = false;

    // Grid layout mode for multi-image Split/Difference display.
    // 0 = Auto, 1 = SingleRow, 2 = SingleCol, 3 = RowsCols
    int grid_layout = 0;
    int grid_cols = 3;

    // Difference-mode options.
    // heatmap_color: 0 = Gray, 1 = Inferno, 2 = Viridis, 3 = Coolwarm
    int heatmap_color = 1;
    double diff_amplification = 5.0;

    // Super-resolution options.
    // Last-used parameters for the SR dialog; persisted so the user
    // does not need to re-enter scale, tile size, etc. every time.
    int sr_scale = 2;           // 2 or 4
    int sr_tile_size = 256;     // Tile size in pixels
    int sr_tile_overlap = 64;   // Overlap between adjacent tiles
    std::string sr_model = "seedvr2_ema_3b-Q4_K_M.gguf";
    std::string sr_color_correction = "lab";

    // Returns the platform-appropriate path to the settings file.
    // Resolves to:
    //   $XDG_CONFIG_HOME/idiff/settings.txt  (or $HOME/.config/idiff/settings.txt)
    //   $HOME/Library/Application Support/idiff/settings.txt  on macOS
    //   %APPDATA%/idiff/settings.txt         on Windows
    // Falls back to "./idiff_settings.txt" in the current directory if
    // no suitable location can be resolved.
    static std::string default_path();

    // Load settings from `path` (or default_path() when empty).  Missing
    // file is not an error; the returned AppSettings is default-constructed.
    static AppSettings load(const std::string& path = {});

    // Write to `path` (or default_path() when empty).  Returns false on
    // I/O failure; last_error contains a human-readable message.
    bool save(const std::string& path = {}) const;

    mutable std::string last_error;
};

} // namespace idiff

#endif // IDIFF_APP_SETTINGS_H
