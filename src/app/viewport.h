#ifndef IDIFF_VIEWPORT_H
#define IDIFF_VIEWPORT_H

#include <imgui.h>

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

class Viewport {
public:
    Viewport();
    ~Viewport() = default;

    void render(const std::vector<SDL_Texture*>& tex_ptrs,
                const std::vector<int>& tex_ws,
                const std::vector<int>& tex_hs,
                const std::vector<const char*>& labels,
                SDL_Texture* tex_diff = nullptr,
                int tex_diff_w = 0, int tex_diff_h = 0);

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

private:
    static ImTextureID to_tex_id(SDL_Texture* tex);

    void draw_image_label(const char* label, ImVec2 img_pos, ImVec2 img_size);
    void draw_selection_rect();

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
    void render_difference(SDL_Texture* tex_diff, int tex_diff_w, int tex_diff_h,
                           const std::vector<const char*>& labels);

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
};

} // namespace idiff

#endif // IDIFF_VIEWPORT_H
