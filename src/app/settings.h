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
