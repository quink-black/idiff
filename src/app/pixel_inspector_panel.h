#ifndef IDIFF_APP_PIXEL_INSPECTOR_PANEL_H
#define IDIFF_APP_PIXEL_INSPECTOR_PANEL_H

#include <string>
#include <utility>
#include <vector>

namespace idiff {

class Image;

// One sample point in the pixel inspector.  The single source of
// truth is the normalized coordinate (u, v) in [0, 1) -- this lets a
// single point describe "the same spot" across images of different
// resolutions.  The UI only ever shows integer pixel coordinates,
// computed from (u, v) against a reference resolution (typically A's
// cols x rows).
struct PixelSamplePoint {
    double u = 0.5;
    double v = 0.5;
    // Set to true for one frame after a coordinate clamp adjustment so
    // the renderer can highlight the row.  Reset at the top of render().
    bool clamp_marker = false;
};

// Inputs forwarded from the inspector when rendering the Pixel sub-panel.
// Pointers are non-owning views; the caller (inspector.cpp) keeps them
// valid for the duration of the render call.
struct PixelInspectorInputs {
    // Ordered list of (label, image) pairs to display.  The first entry
    // is treated as A and used as the delta reference and the default
    // reference resolution.  Order should mirror the viewport cell
    // order so the table layout matches what is on screen.
    std::vector<std::pair<std::string, const Image*>> samples;
};

// Stateful inspector panel.  Owns the pinned-sample list and the most
// recent hover sample, but otherwise pulls everything it needs from
// PixelInspectorInputs at render time.  No SDL/OpenGL state is held;
// the unit tests construct this without an ImGui context.
class PixelInspectorPanel {
public:
    PixelInspectorPanel();
    ~PixelInspectorPanel();

    // Refresh the per-frame hover sample.  When `valid` is false the
    // hover row keeps its label but the coord/value/delta cells render
    // as em-dashes.
    void update_hover(double u, double v, bool valid);

    // Append the currently-hovered sample to the pinned list.  No-op
    // when the cursor is not over the viewport (hover_valid is false).
    // Intended to be called from the viewport's own hotkey handler
    // (e.g. Alt+P) where the cursor is, by construction, still over
    // the image -- there is no "button on the inspector" path because
    // moving the mouse to such a button would invariably invalidate
    // the hover before the click landed.
    void pin_current_hover();

    // Append an arbitrary (u, v) pair to the pinned list.  Used by the
    // viewport Shift+Click shortcut.  Coordinates are clamped into the
    // valid normalized range first.
    void pin_at(double u, double v);

    // Remove all pinned samples.  Hover row is unaffected.
    void clear_pins();

    // Number of pinned samples (excluding the hover row).  Exposed for
    // tests / debug logging.
    int pin_count() const noexcept { return static_cast<int>(pinned_.size()); }

    // Render the panel.  Safe to call with an empty `inputs.samples`;
    // the renderer falls back to a placeholder message instead.
    void render(const PixelInspectorInputs& inputs);

private:
    PixelSamplePoint hover_{};
    bool hover_valid_ = false;
    std::vector<PixelSamplePoint> pinned_;
};

} // namespace idiff

#endif // IDIFF_APP_PIXEL_INSPECTOR_PANEL_H
