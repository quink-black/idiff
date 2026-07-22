#ifndef IDIFF_APP_SETTINGS_H
#define IDIFF_APP_SETTINGS_H

#include <string>
#include <vector>

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

    // Inspector panel currently shown.
    // 0 = Properties (default), 1 = Pixel, 2 = Metrics,
    // 3 = Statistics, 4 = Measurements.  Persisted so the user does
    // not have to reselect their preferred inspector tab on every launch.
    int inspector_panel = 0;

    // Panel visibility.  Persisted so the workspace layout survives
    // restarts.
    bool show_image_list = true;
    bool show_inspector = true;

    // Image grouping.  When true, clicking an entry selects all
    // entries sharing the same filename stem.
    bool group_by_name = true;

    // Upscale method: 0 = Nearest, 1 = Bilinear, 2 = Bicubic,
    // 3 = Lanczos.
    int upscale_method = 3;

    // Channel view mode: 0 = None, 1 = RGB, 2 = R, 3 = G, 4 = B,
    // 5 = AlphaGray, 6 = AlphaContour, 7 = Y, 8 = U, 9 = V.
    int channel_view_mode = 0;

    // View background: 0 = Black, 1 = White, 2 = Red, 3 = Green,
    // 4 = Blue, 5 = DarkChecker, 6 = LightChecker.
    int view_background = 5;

    // Comparison mode: 0 = Split, 1 = Overlay, 2 = Difference.
    int comparison_mode = 0;

    // Image loader backend: 0 = ImageMagick, 1 = OpenCV, 2 = FFmpeg.
    int loader_backend = 0;

    // LRU cache capacity: number of recently-deselected entries whose
    // pixels stay in memory to avoid re-decode on brief toggles.
    // Default 20 ≈ 660 MB for 4K RGBA8 images (3840x2160x4), ~160 MB
    // for 1080p (1920x1080x4).
    int lru_capacity = 20;

    // Media paths to reload on next launch.  Populated by the Restart
    // menu item, consumed and cleared by main() at startup.  Empty for
    // a normal launch.
    std::vector<std::string> session_paths;

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
