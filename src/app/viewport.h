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
};

} // namespace idiff

#endif // IDIFF_VIEWPORT_H
