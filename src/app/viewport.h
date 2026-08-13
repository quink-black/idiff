#ifndef IDIFF_VIEWPORT_H
#define IDIFF_VIEWPORT_H

#include <imgui.h>

#include "app/measurement.h"
#include "app/texture_types.h"
#include "core/channel_view.h"

#include <cstdint>
#include <string>
#include <vector>

struct SDL_Texture;

namespace idiff {

enum class ComparisonMode {
    Split,
    Overlay,
    Difference,
};

enum class GridLayout {
    Auto,       // Current heuristic
    SingleRow,  // 1xN (all images in one horizontal row)
    SingleCol,  // Nx1 (all images in one vertical column)
    RowsCols,   // User-specified columns; rows derived
};

struct TextureTileView {
    SDL_Texture* texture = nullptr;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

class Viewport {
public:
    Viewport();
    ~Viewport() = default;

    // tex_ws/tex_hs are the source image's native pixel dimensions for
    // each slot.  They are the coordinate system for rulers, hover
    // pixel readouts, and measurement rectangles.  Callers must not
    // pass the SDL texture's allocated size when the texture has been
    // resampled (e.g. upscaled to match a higher-resolution partner);
    // doing so would inflate measurement values relative to the
    // original image.
    void render(const std::vector<SDL_Texture*>& tex_ptrs,
                const std::vector<int>& tex_ws,
                const std::vector<int>& tex_hs,
                const std::vector<const char*>& labels,
                const std::vector<SDL_Texture*>& diff_tex_ptrs = {},
                const std::vector<int>& diff_tex_ws = {},
                const std::vector<int>& diff_tex_hs = {},
                const std::vector<const char*>& diff_labels = {},
                const std::vector<std::vector<TextureTileView>>& tiles = {},
                const std::vector<std::vector<TextureTileView>>& diff_tiles = {});

    const std::vector<VisibleImageRegion>& visible_regions() const noexcept {
        return visible_regions_;
    }

    ComparisonMode mode() const noexcept { return mode_; }
    void set_mode(ComparisonMode mode) { mode_ = mode; }

    float zoom() const noexcept { return zoom_; }
    void set_zoom(float z);

    float pan_x() const noexcept { return pan_x_; }
    float pan_y() const noexcept { return pan_y_; }
    void set_pan(float x, float y) { pan_x_ = x; pan_y_ = y; }

    // Zoom keeping a screen-space anchor point fixed.
    // anchor is in screen coordinates (e.g. mouse position).
    void zoom_around(float new_zoom, ImVec2 anchor);

    // Zoom to fit a screen-space rectangle into the viewport.
    void zoom_to_rect(ImVec2 rect_min, ImVec2 rect_max);

    // Reset zoom and pan so the content fits the viewport.
    void fit_to_content();

    // Reset to 1:1 pixel zoom centered.
    void zoom_to_actual();

    // Selection rectangle state (screen coords, driven by App input handling)
    bool selecting() const noexcept { return selecting_; }
    void begin_selection(ImVec2 start);
    void update_selection(ImVec2 current);
    void end_selection();   // commits zoom_to_rect
    void cancel_selection();

    ImVec2 selection_min() const noexcept { return sel_min_; }
    ImVec2 selection_max() const noexcept { return sel_max_; }

    // Last known viewport region (set during render)
    ImVec2 viewport_origin() const noexcept { return vp_origin_; }
    ImVec2 viewport_size() const noexcept { return vp_size_; }

    // Overlay split position (0..1, left vs right half).  Exposed so the
    // "Save viewport" feature can reproduce the same split in the saved
    // image.
    float overlay_slider_pos() const noexcept { return slider_pos_; }

    // Set the A/B overlay split position directly (0..1).  Out-of-range
    // values are clamped.  The user normally drives this by dragging
    // the on-screen slider; this setter exists for programmatic
    // control (RPC view.screenshot, future preset playback, tests).
    void set_overlay_slider_pos(float v);

    // True when the overlay A/B slider is actively being dragged by the
    // user (set during render_overlay, valid until the next render call).
    bool overlay_slider_dragging() const noexcept { return slider_dragging_; }

    // Hover info computed during render().  Valid until the next render()
    // call.  Consumers should check hover_valid() before using the values.
    //
    // In Split mode, hover_cell_index() identifies which texture slot
    // (0..N-1, matching the tex_ptrs order passed to render) is under
    // the cursor.  In Overlay / Difference modes the cell index is always
    // 0 (overlay uses the composite) and the returned pixel coordinates
    // are in the composite image's coordinate space.
    bool hover_valid() const noexcept { return hover_valid_; }
    int hover_cell_index() const noexcept { return hover_cell_idx_; }
    int hover_pixel_x() const noexcept { return hover_px_x_; }
    int hover_pixel_y() const noexcept { return hover_px_y_; }

    // Ruler and grid overlay toggles
    bool show_ruler() const noexcept { return show_ruler_; }
    void set_show_ruler(bool v) { show_ruler_ = v; }
    bool show_grid() const noexcept { return show_grid_; }
    void set_show_grid(bool v) { show_grid_ = v; }

    // Grid layout for multi-image Split/Difference modes
    GridLayout grid_layout() const noexcept { return grid_layout_; }
    void set_grid_layout(GridLayout v) { grid_layout_ = v; }
    int grid_cols() const noexcept { return grid_cols_; }
    void set_grid_cols(int v) { grid_cols_ = std::max(1, v); }

    // Single-channel view mode
    ChannelViewMode channel_view_mode() const noexcept { return channel_view_mode_; }
    void set_channel_view_mode(ChannelViewMode v) { channel_view_mode_ = v; }

    // Background for RGBA compositing
    ViewBackground view_background() const noexcept { return view_bg_; }
    void set_view_background(ViewBackground v) { view_bg_ = v; }

    // Compute grid dimensions for `n` items under the given layout.
    // In RowsCols mode, `user_cols` specifies the column count and rows
    // are derived; ignored for other modes.
    static void compute_grid(int n, GridLayout layout, int user_cols,
                             int& cols, int& rows);

    // --- Measurement mode ---
    //
    // When measure mode is on, App routes left-drag input to the
    // begin/update/end_measurement APIs instead of panning.  Each saved
    // measurement records a rectangle in the source image's native pixel
    // coordinates; labels and the right-side panel use that rectangle to
    // display "W x H px" at image resolution rather than screen pixels.
    bool  measure_mode() const noexcept { return measure_mode_; }
    void  set_measure_mode(bool on) { measure_mode_ = on; }

    // Reset the monotonically-assigned id counter (used after loading
    // saved measurements from entries so new ids do not collide).
    void  set_next_measurement_id(int id) { next_measurement_id_ = id; }

    // Begin a measurement drag anchored at `screen_pt`.  The cell under
    // that point, and that cell's source texture dimensions, are captured
    // once and never revisited for this drag.  If the point does not fall
    // on a valid image cell the call is a no-op.
    void  begin_measurement(ImVec2 screen_pt);
    void  update_measurement(ImVec2 screen_pt);
    // Commit the in-progress measurement.  Rectangles smaller than a
    // pixel in either dimension are discarded (treated as a stray click).
    // Returns pointer to the committed measurement (valid until the next
    // measurement mutation), or nullptr if the drag was too small.
    const Measurement* end_measurement();
    void  cancel_measurement();
    bool  measuring() const noexcept { return measuring_; }

    void  remove_measurement(int id);
    void  clear_measurements();
    // Insert a pre-built measurement (e.g. loaded from an ImageEntry on
    // selection change).  Does not affect next_measurement_id_.
    void  add_measurement(const Measurement& m);
    const std::vector<Measurement>& measurements() const noexcept {
        return measurements_;
    }

    // Returns the currently hovered measurement id, or -1 if none.
    // Populated during render() and valid until the next render() call.
    int   hover_measurement_id() const noexcept { return hover_measurement_id_; }
    // True when the mouse currently sits on a saved measurement's x
    // (delete) button.  Apps should suppress begin_measurement on left
    // click in that case so the click deletes the rect without also
    // starting a new drag.
    bool  hover_measurement_close_hot() const noexcept { return hover_close_hot_; }

    // Returns the id of the measurement deleted via the x button during
    // the last render(), or -1 if none.  Resets the flag so the same
    // deletion is not reported twice.  Callers should also remove the
    // measurement from the owning ImageEntry to keep them in sync.
    int   consume_pending_delete() {
        int id = last_deleted_id_;
        last_deleted_id_ = -1;
        return id;
    }

private:
    static ImTextureID to_tex_id(SDL_Texture* tex);

    void draw_image_label(const char* label,
                           ImVec2 img_pos, ImVec2 img_size,
                           ImVec2 cell_pos, ImVec2 cell_size,
                           bool ruler_visible);
    void draw_selection_rect();
    void draw_tiled_image(SDL_Texture* proxy,
                          const std::vector<TextureTileView>& tiles,
                          int source_w, int source_h,
                          ImVec2 image_pos, ImVec2 image_size,
                          ImVec2 clip_min, ImVec2 clip_max,
                          int slot, bool difference);

    // --- Measurement helpers ---
    //
    // Resolve the screen point into the cell / texture it hovers.
    // Returns false when no valid image sits under the point (outside the
    // viewport, empty cell, unsupported mode).  On success, `cell_origin`
    // and `cell_size` describe the cell rect in screen coords, and
    // `tex_w`, `tex_h` are the source image's native dimensions for that
    // cell.  Works across Split / Overlay / Difference modes, mirroring
    // the hover pixel computation.
    bool resolve_cell_at(ImVec2 screen_pt,
                         int& cell_index,
                         int& tex_w, int& tex_h,
                         ImVec2& cell_origin, ImVec2& cell_size) const;

    // Map a screen point to the source-image pixel coordinates of the
    // given cell using the cell's frozen layout info.  Output may fall
    // outside [0..tex_w/h) when the user drags beyond the cell edge; the
    // caller is free to keep those values (measurement extrapolation) or
    // clamp.
    void screen_to_image_px(ImVec2 screen_pt,
                            int cell_index,
                            int tex_w, int tex_h,
                            ImVec2 cell_origin, ImVec2 cell_size,
                            float& out_px, float& out_py) const;

    // Map source-image pixel coordinates back to the current screen
    // position using the live viewport pan/zoom.  Used to redraw saved
    // measurements every frame.
    void image_px_to_screen(int cell_index,
                            int tex_w, int tex_h,
                            float px, float py,
                            ImVec2& out_screen) const;

    // Draw in-progress + saved measurements on top of the viewport.
    // Populates hover_measurement_id_ and handles the x (delete) button.
    void draw_measurements();

    // Compute the top-left screen position and displayed size for a single image
    // given the viewport area, applying zoom and pan.
    void compute_image_rect(int img_w, int img_h,
                            ImVec2& out_pos, ImVec2& out_size) const;

    void render_split(const std::vector<SDL_Texture*>& tex_ptrs,
                      const std::vector<int>& tex_ws,
                      const std::vector<int>& tex_hs,
                      const std::vector<const char*>& labels);
    void render_overlay(const std::vector<SDL_Texture*>& tex_ptrs,
                        const std::vector<int>& tex_ws,
                        const std::vector<int>& tex_hs,
                        const std::vector<const char*>& labels);
    void render_difference(const std::vector<SDL_Texture*>& diff_tex_ptrs,
                           const std::vector<int>& diff_tex_ws,
                           const std::vector<int>& diff_tex_hs,
                           const std::vector<const char*>& diff_labels);

    // Ruler and grid drawing
    // img_pos: top-left screen corner of the displayed image
    // img_size: displayed image size in screen pixels
    // img_w/img_h: original image dimensions in pixels
    // scale: display pixels per original image pixel
    void draw_ruler(ImVec2 img_pos, ImVec2 img_size, int img_w, int img_h, float scale,
                    ImVec2 cell_origin, ImVec2 cell_size);
    void draw_grid(ImVec2 img_pos, ImVec2 img_size, int img_w, int img_h, float scale);

    ComparisonMode mode_ = ComparisonMode::Split;
    float zoom_ = 1.0f;
    float pan_x_ = 0.0f;
    float pan_y_ = 0.0f;
    float split_pos_ = 0.5f;
    float slider_pos_ = 0.5f;

    // Selection rectangle (screen coords)
    bool selecting_ = false;
    ImVec2 sel_start_{0, 0};
    ImVec2 sel_min_{0, 0};
    ImVec2 sel_max_{0, 0};

    // Viewport region recorded during render
    ImVec2 vp_origin_{0, 0};
    ImVec2 vp_size_{0, 0};

    // Content dimensions (max image size across selected images)
    int content_w_ = 0;
    int content_h_ = 0;

    // Split layout: cell size recorded during render_split.
    // For non-split modes, cell == viewport.
    int split_cols_ = 1;
    int split_rows_ = 1;

    // Return the cell origin and size for a given screen-space point.
    // In split mode, identifies which cell the point falls in.
    // In other modes, returns the full viewport.
    void cell_at(ImVec2 screen_pt, ImVec2& out_origin, ImVec2& out_size) const;

    // Per-slot texture dimensions captured during the latest render() pass.
    // Indexed by slot index, which matches the cell index in Split and
    // Difference modes.  In Overlay mode the grid only has one cell on
    // screen, but two slots (A and B) share it via the A/B slider, so
    // resolve_cell_at returns the slot index based on which side of the
    // slider the cursor is on.
    //
    // tex_w/tex_h are the source image's native pixel dimensions (used as
    // the measurement coordinate system, so the reported W x H reflects
    // that source's resolution).  composite_w/composite_h are the pixel
    // dimensions of the rect that the slot's source image is drawn into
    // on screen: in Overlay mode this is max(A, B) because both sources
    // are stretched onto a shared composite canvas; in Split/Difference
    // it equals tex_w/tex_h.  Screen projection uses composite_w/h so
    // measurements stay aligned with the visible pixels regardless of
    // the per-source resolution mismatch.
    struct CellLayout {
        int tex_w = 0;
        int tex_h = 0;
        int composite_w = 0;
        int composite_h = 0;
        int grid_cell = 0;     // which on-screen grid cell this slot occupies
    };
    std::vector<CellLayout> cell_layouts_;
    const std::vector<std::vector<TextureTileView>>* frame_tiles_ = nullptr;
    const std::vector<std::vector<TextureTileView>>* frame_diff_tiles_ = nullptr;
    std::vector<VisibleImageRegion> visible_regions_;

    // Hover state populated during render()
    bool hover_valid_ = false;
    int hover_cell_idx_ = -1;
    int hover_px_x_ = 0;
    int hover_px_y_ = 0;

    // True when the overlay slider InvisibleButton is being dragged
    bool slider_dragging_ = false;

    // Overlay toggles
    bool show_ruler_ = false;
    bool show_grid_ = false;

    // Grid layout
    GridLayout grid_layout_ = GridLayout::Auto;
    int grid_cols_ = 3;

    // Single-channel view mode applied to all displayed images.
    ChannelViewMode channel_view_mode_ = ChannelViewMode::None;

    // Background used when compositing RGBA images.
    ViewBackground view_bg_ = ViewBackground::DarkChecker;

    // --- Measurement state ---
    bool measure_mode_ = false;
    bool measuring_ = false;
    // Cell/tex info frozen at mouse-down so extrapolation past cell edges
    // stays consistent with the source image's coordinate system.
    int   meas_drag_cell_ = -1;
    int   meas_drag_tex_w_ = 0;
    int   meas_drag_tex_h_ = 0;
    float meas_drag_x0_ = 0.0f;
    float meas_drag_y0_ = 0.0f;
    float meas_drag_x1_ = 0.0f;
    float meas_drag_y1_ = 0.0f;

    std::vector<Measurement> measurements_;
    int next_measurement_id_ = 1;

    // Saved in render() so measurement APIs invoked between frames see
    // the last frame's layout without re-running the full render path.
    int hover_measurement_id_ = -1;
    bool hover_close_hot_ = false;

    // Pending delete requested by the x button during render(); applied
    // at the end of draw_measurements() to avoid invalidating iterators.
    int pending_delete_id_ = -1;
    // The id of the measurement most recently removed by the x button,
    // exposed via consume_pending_delete() so App can sync the deletion
    // to the owning ImageEntry.
    int last_deleted_id_ = -1;
};

} // namespace idiff

#endif // IDIFF_VIEWPORT_H
