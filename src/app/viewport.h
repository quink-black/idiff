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
    void set_zoom(float z) { zoom_ = z; }

    float pan_x() const noexcept { return pan_x_; }
    float pan_y() const noexcept { return pan_y_; }
    void set_pan(float x, float y) { pan_x_ = x; pan_y_ = y; }

private:
    static ImTextureID to_tex_id(SDL_Texture* tex);

    void draw_image_label(const char* label, ImVec2 img_pos, ImVec2 img_size);

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
};

} // namespace idiff

#endif // IDIFF_VIEWPORT_H
