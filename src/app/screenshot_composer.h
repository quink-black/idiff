// Pure renderer for "what the viewport currently shows" -> a single
// cv::Mat in BGRA-8.
//
// Extracted from App::save_viewport_dialog() so the same composition
// logic can be reused by the JSON-RPC view.screenshot method without
// dragging in the file dialog / file watcher / status bar plumbing
// that the GUI Save flow needs around it.
//
// The composer is a free function that takes everything it needs by
// const reference and returns the composed image (or an empty Mat on
// failure).  It does no I/O and never touches the status reporter:
// callers are responsible for producing user-visible error messages
// and writing the file to disk.
//
// Why a free function rather than a method on Viewport?  The
// composition reads from ImageEntry / DiffSlot data structures owned
// by App, not from any state held inside Viewport itself -- the
// viewport just orchestrates ImGui draw calls.  Pulling the function
// onto Viewport would force Viewport to know about ImageEntry, which
// inverts the intended dependency direction.

#ifndef IDIFF_APP_SCREENSHOT_COMPOSER_H
#define IDIFF_APP_SCREENSHOT_COMPOSER_H

#include "app/app.h"        // ImageEntry, DiffSlot
#include "app/viewport.h"   // ComparisonMode, GridLayout

#include <opencv2/core.hpp>

#include <set>
#include <string>
#include <vector>

namespace idiff {

class DiffService;

struct ComposeViewportInput {
    ComparisonMode mode = ComparisonMode::Split;

    // Selection layout -- ref_idx is the entry that goes first
    // ("[Ref] ...").  Both indices are interpreted against `entries`.
    int ref_idx = -1;
    const std::set<int>* selection = nullptr;
    const std::vector<ImageEntry>* entries = nullptr;

    // Difference-mode source (one heatmap per partner).  Only consulted
    // when mode == Difference.  May be null in other modes.
    const DiffService* diff = nullptr;

    // Overlay split position (0..1).  Only consulted when mode ==
    // Overlay.
    float overlay_slider_pos = 0.5f;

    // Grid layout for Split / Difference modes.
    GridLayout grid_layout = GridLayout::Auto;
    int grid_cols = 1;
};

// Returns the composed BGRA-8 image, or an empty Mat on failure.
// `error_message` (when non-null) is populated with a human-readable
// reason on failure; on success it is left untouched.
cv::Mat compose_viewport(const ComposeViewportInput& in,
                         std::string* error_message);

} // namespace idiff

#endif // IDIFF_APP_SCREENSHOT_COMPOSER_H
