#include "app/viewport.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace idiff {

Viewport::Viewport() = default;

ImTextureID Viewport::to_tex_id(SDL_Texture* tex) {
    return (ImTextureID)(uintptr_t)tex;
}

// --- Zoom / pan helpers ---

void Viewport::set_zoom(float z) {
    zoom_ = std::clamp(z, 0.05f, 64.0f);
}

void Viewport::zoom_around(float new_zoom, ImVec2 anchor) {
    new_zoom = std::clamp(new_zoom, 0.05f, 64.0f);
    if (new_zoom == zoom_) return;

    ImVec2 cell_org, cell_sz;
    cell_at(anchor, cell_org, cell_sz);

    float cx = cell_org.x + cell_sz.x * 0.5f + pan_x_;
    float cy = cell_org.y + cell_sz.y * 0.5f + pan_y_;

    float ratio = 1.0f - new_zoom / zoom_;
    pan_x_ += (anchor.x - cx) * ratio;
    pan_y_ += (anchor.y - cy) * ratio;

    zoom_ = new_zoom;
}

void Viewport::zoom_to_rect(ImVec2 rect_min, ImVec2 rect_max) {
    float rw = rect_max.x - rect_min.x;
    float rh = rect_max.y - rect_min.y;
    if (rw < 4.0f || rh < 4.0f || vp_size_.x < 1.0f || vp_size_.y < 1.0f) return;

    ImVec2 sel_center((rect_min.x + rect_max.x) * 0.5f,
                      (rect_min.y + rect_max.y) * 0.5f);
    ImVec2 cell_org, cell_sz;
    cell_at(sel_center, cell_org, cell_sz);

    float sel_cx = sel_center.x;
    float sel_cy = sel_center.y;

    float cell_cx = cell_org.x + cell_sz.x * 0.5f;
    float cell_cy = cell_org.y + cell_sz.y * 0.5f;

    float scale_factor = std::min(cell_sz.x / rw, cell_sz.y / rh);

    float new_zoom = std::clamp(zoom_ * scale_factor, 0.05f, 64.0f);
    float actual_factor = new_zoom / zoom_;

    pan_x_ = -(sel_cx - cell_cx - pan_x_) * actual_factor;
    pan_y_ = -(sel_cy - cell_cy - pan_y_) * actual_factor;
    zoom_ = new_zoom;
}

void Viewport::fit_to_content() {
    zoom_ = 1.0f;
    pan_x_ = 0.0f;
    pan_y_ = 0.0f;
}

void Viewport::zoom_to_actual() {
    if (content_w_ <= 0 || content_h_ <= 0 || vp_size_.x <= 0 || vp_size_.y <= 0) {
        zoom_ = 1.0f;
        pan_x_ = 0.0f;
        pan_y_ = 0.0f;
        return;
    }

    float area_w = vp_size_.x / split_cols_;
    float area_h = vp_size_.y / split_rows_;
    float fit_scale = std::min(area_w / content_w_, area_h / content_h_);
    float new_zoom = 1.0f / fit_scale;
    pan_x_ = 0.0f;
    pan_y_ = 0.0f;
    zoom_ = std::clamp(new_zoom, 0.05f, 64.0f);
}

// --- Selection rectangle ---

void Viewport::begin_selection(ImVec2 start) {
    selecting_ = true;
    sel_start_ = start;
    sel_min_ = start;
    sel_max_ = start;
}

void Viewport::update_selection(ImVec2 current) {
    sel_min_ = ImVec2(std::min(sel_start_.x, current.x), std::min(sel_start_.y, current.y));
    sel_max_ = ImVec2(std::max(sel_start_.x, current.x), std::max(sel_start_.y, current.y));
}

void Viewport::end_selection() {
    selecting_ = false;
    zoom_to_rect(sel_min_, sel_max_);
}

void Viewport::cancel_selection() {
    selecting_ = false;
}

void Viewport::draw_selection_rect() {
    if (!selecting_) return;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRect(sel_min_, sel_max_, IM_COL32(255, 200, 50, 220), 0.0f, 0, 2.0f);
    dl->AddRectFilled(sel_min_, sel_max_, IM_COL32(255, 200, 50, 30));
}

// --- Drawing helpers ---

void Viewport::draw_image_label(const char* label, ImVec2 anchor_pos, ImVec2 /*anchor_size*/) {
    ImVec2 text_size = ImGui::CalcTextSize(label);
    float pad_x = 6.0f, pad_y = 4.0f;

    ImVec2 rect_min(anchor_pos.x + 4, anchor_pos.y + 4);
    ImVec2 rect_max(rect_min.x + text_size.x + pad_x * 2,
                    rect_min.y + text_size.y + pad_y * 2);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(rect_min, rect_max,
                      IM_COL32(0, 0, 0, 180), 3.0f);
    dl->AddText(ImVec2(rect_min.x + pad_x, rect_min.y + pad_y),
                IM_COL32(255, 255, 255, 230), label);
}

void Viewport::compute_image_rect(int img_w, int img_h,
                                   ImVec2& out_pos, ImVec2& out_size) const {
    float fit_scale = std::min(vp_size_.x / img_w, vp_size_.y / img_h);
    float scale = fit_scale * zoom_;

    float disp_w = img_w * scale;
    float disp_h = img_h * scale;

    float x = vp_origin_.x + (vp_size_.x - disp_w) * 0.5f + pan_x_;
    float y = vp_origin_.y + (vp_size_.y - disp_h) * 0.5f + pan_y_;

    out_pos = ImVec2(x, y);
    out_size = ImVec2(disp_w, disp_h);
}

// --- Ruler and Grid ---

int Viewport::compute_nice_interval(float scale, float min_screen_spacing) {
    // Use 1-2-5 sequence to find the smallest interval whose screen size
    // is at least min_screen_spacing pixels.
    static const int bases[] = {1, 2, 5};
    int magnitude = 1;
    while (true) {
        for (int b : bases) {
            int interval = b * magnitude;
            if (interval * scale >= min_screen_spacing) {
                return interval;
            }
        }
        magnitude *= 10;
    }
}

void Viewport::draw_ruler(ImVec2 img_pos, ImVec2 img_size,
                           int img_w, int img_h, float scale) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    constexpr float ruler_thickness = 18.0f;
    constexpr float min_tick_spacing = 50.0f;
    ImU32 bg_color = IM_COL32(40, 40, 40, 200);
    ImU32 tick_color = IM_COL32(180, 180, 180, 220);
    ImU32 label_color = IM_COL32(200, 200, 200, 255);
    ImU32 border_color = IM_COL32(80, 80, 80, 255);

    int interval = compute_nice_interval(scale, min_tick_spacing);

    // Horizontal ruler (top edge)
    {
        float ruler_y = img_pos.y - ruler_thickness;
        dl->AddRectFilled(ImVec2(img_pos.x, ruler_y),
                          ImVec2(img_pos.x + img_size.x, img_pos.y),
                          bg_color);
        dl->AddLine(ImVec2(img_pos.x, img_pos.y),
                    ImVec2(img_pos.x + img_size.x, img_pos.y),
                    border_color);

        int start_px = 0;
        int end_px = img_w;
        for (int px = start_px; px <= end_px; px += interval) {
            float sx = img_pos.x + px * scale;

            // Major tick
            dl->AddLine(ImVec2(sx, img_pos.y - 8),
                        ImVec2(sx, img_pos.y),
                        tick_color);

            // Label
            if (px > 0 || true) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%d", px);
                dl->AddText(ImVec2(sx + 2, ruler_y + 1), label_color, buf);
            }
        }

        // Minor ticks at interval/5
        int minor = std::max(1, interval / 5);
        for (int px = 0; px <= end_px; px += minor) {
            if (px % interval == 0) continue;
            float sx = img_pos.x + px * scale;
            dl->AddLine(ImVec2(sx, img_pos.y - 4),
                        ImVec2(sx, img_pos.y),
                        IM_COL32(120, 120, 120, 180));
        }
    }

    // Vertical ruler (left edge)
    {
        float ruler_x = img_pos.x - ruler_thickness;
        dl->AddRectFilled(ImVec2(ruler_x, img_pos.y),
                          ImVec2(img_pos.x, img_pos.y + img_size.y),
                          bg_color);
        dl->AddLine(ImVec2(img_pos.x, img_pos.y),
                    ImVec2(img_pos.x, img_pos.y + img_size.y),
                    border_color);

        int start_py = 0;
        int end_py = img_h;
        for (int py = start_py; py <= end_py; py += interval) {
            float sy = img_pos.y + py * scale;

            dl->AddLine(ImVec2(img_pos.x - 8, sy),
                        ImVec2(img_pos.x, sy),
                        tick_color);

            char buf[32];
            std::snprintf(buf, sizeof(buf), "%d", py);
            dl->AddText(ImVec2(ruler_x + 2, sy + 1), label_color, buf);
        }

        int minor = std::max(1, interval / 5);
        for (int py = 0; py <= end_py; py += minor) {
            if (py % interval == 0) continue;
            float sy = img_pos.y + py * scale;
            dl->AddLine(ImVec2(img_pos.x - 4, sy),
                        ImVec2(img_pos.x, sy),
                        IM_COL32(120, 120, 120, 180));
        }
    }

    // Corner square
    dl->AddRectFilled(ImVec2(img_pos.x - ruler_thickness, img_pos.y - ruler_thickness),
                      ImVec2(img_pos.x, img_pos.y),
                      bg_color);
}

void Viewport::draw_grid(ImVec2 img_pos, ImVec2 img_size,
                          int img_w, int img_h, float scale) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    constexpr float min_line_spacing = 40.0f;
    ImU32 grid_color = IM_COL32(255, 255, 255, 30);

    int interval = compute_nice_interval(scale, min_line_spacing);

    // Vertical lines
    for (int px = interval; px < img_w; px += interval) {
        float sx = img_pos.x + px * scale;
        if (sx < img_pos.x || sx > img_pos.x + img_size.x) continue;
        dl->AddLine(ImVec2(sx, img_pos.y),
                    ImVec2(sx, img_pos.y + img_size.y),
                    grid_color);
    }

    // Horizontal lines
    for (int py = interval; py < img_h; py += interval) {
        float sy = img_pos.y + py * scale;
        if (sy < img_pos.y || sy > img_pos.y + img_size.y) continue;
        dl->AddLine(ImVec2(img_pos.x, sy),
                    ImVec2(img_pos.x + img_size.x, sy),
                    grid_color);
    }
}

// --- Render ---

void Viewport::render(const std::vector<SDL_Texture*>& tex_ptrs,
                      const std::vector<int>& tex_ws,
                      const std::vector<int>& tex_hs,
                      const std::vector<const char*>& labels,
                      SDL_Texture* tex_diff, int tex_diff_w, int tex_diff_h) {
    ImVec2 avail = ImGui::GetContentRegionAvail();
    vp_origin_ = ImGui::GetCursorScreenPos();
    vp_size_ = avail;

    hover_valid_ = false;
    hover_cell_idx_ = -1;
    hover_px_x_ = 0;
    hover_px_y_ = 0;

    if (avail.x < 10 || avail.y < 10) return;

    content_w_ = 0;
    content_h_ = 0;
    for (size_t i = 0; i < tex_ws.size(); i++) {
        content_w_ = std::max(content_w_, tex_ws[i]);
        content_h_ = std::max(content_h_, tex_hs[i]);
    }
    if (tex_diff) {
        content_w_ = std::max(content_w_, tex_diff_w);
        content_h_ = std::max(content_h_, tex_diff_h);
    }

    if (tex_ptrs.empty() && !tex_diff) {
        ImVec2 center(avail.x * 0.5f, avail.y * 0.5f);
        ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + center.x - 100,
                                    ImGui::GetCursorPosY() + center.y - 30));
        ImGui::TextDisabled("No images selected");
        ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + center.x - 140,
                                    ImGui::GetCursorPosY() + 8));
        ImGui::TextDisabled("Check images in the Images panel to compare");
        return;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(vp_origin_, ImVec2(vp_origin_.x + vp_size_.x,
                                         vp_origin_.y + vp_size_.y), true);

    if (mode_ != ComparisonMode::Split) {
        split_cols_ = 1;
        split_rows_ = 1;
    }
    slider_dragging_ = false;

    switch (mode_) {
        case ComparisonMode::Split:
            render_split(tex_ptrs, tex_ws, tex_hs, labels);
            break;
        case ComparisonMode::Overlay:
            render_overlay(tex_ptrs, tex_ws, tex_hs, labels);
            break;
        case ComparisonMode::Difference:
            render_difference(tex_diff, tex_diff_w, tex_diff_h, labels);
            break;
    }

    // --- Compute hover pixel info ---
    {
        ImVec2 mp = ImGui::GetIO().MousePos;
        bool inside_vp = mp.x >= vp_origin_.x && mp.x < vp_origin_.x + vp_size_.x &&
                         mp.y >= vp_origin_.y && mp.y < vp_origin_.y + vp_size_.y;
        if (inside_vp) {
            if (mode_ == ComparisonMode::Split && !tex_ptrs.empty()) {
                int n = static_cast<int>(tex_ptrs.size());
                float cell_w = vp_size_.x / split_cols_;
                float cell_h = vp_size_.y / split_rows_;
                int col = static_cast<int>((mp.x - vp_origin_.x) / cell_w);
                int row = static_cast<int>((mp.y - vp_origin_.y) / cell_h);
                col = std::clamp(col, 0, split_cols_ - 1);
                row = std::clamp(row, 0, split_rows_ - 1);
                int idx = row * split_cols_ + col;
                if (idx < n && tex_ptrs[idx] && tex_ws[idx] > 0 && tex_hs[idx] > 0) {
                    float cell_x = vp_origin_.x + col * cell_w;
                    float cell_y = vp_origin_.y + row * cell_h;
                    float fit_scale = std::min(cell_w / tex_ws[idx],
                                               cell_h / tex_hs[idx]);
                    float scale = fit_scale * zoom_;
                    float disp_w = tex_ws[idx] * scale;
                    float disp_h = tex_hs[idx] * scale;
                    float img_x = cell_x + (cell_w - disp_w) * 0.5f + pan_x_;
                    float img_y = cell_y + (cell_h - disp_h) * 0.5f + pan_y_;
                    if (scale > 0.0f) {
                        int px = static_cast<int>((mp.x - img_x) / scale);
                        int py = static_cast<int>((mp.y - img_y) / scale);
                        if (px >= 0 && px < tex_ws[idx] &&
                            py >= 0 && py < tex_hs[idx]) {
                            hover_valid_ = true;
                            hover_cell_idx_ = idx;
                            hover_px_x_ = px;
                            hover_px_y_ = py;
                        }
                    }
                }
            } else if (mode_ == ComparisonMode::Overlay && !tex_ptrs.empty()) {
                int img_w = tex_ws[0];
                int img_h = tex_hs[0];
                int cell = 0;
                if (tex_ptrs.size() >= 2 && tex_ptrs[1]) {
                    img_w = std::max(tex_ws[0], tex_ws[1]);
                    img_h = std::max(tex_hs[0], tex_hs[1]);
                    float slider_screen_x = vp_origin_.x + vp_size_.x * slider_pos_;
                    cell = (mp.x < slider_screen_x) ? 0 : 1;
                }
                if (img_w > 0 && img_h > 0) {
                    ImVec2 pos, size;
                    compute_image_rect(img_w, img_h, pos, size);
                    if (size.x > 0.0f && size.y > 0.0f) {
                        float scale = size.x / img_w;
                        int px = static_cast<int>((mp.x - pos.x) / scale);
                        int py = static_cast<int>((mp.y - pos.y) / scale);
                        int real_w = (cell == 1 && tex_ptrs.size() >= 2)
                                         ? tex_ws[1] : tex_ws[0];
                        int real_h = (cell == 1 && tex_ptrs.size() >= 2)
                                         ? tex_hs[1] : tex_hs[0];
                        if (px >= 0 && px < real_w && py >= 0 && py < real_h) {
                            hover_valid_ = true;
                            hover_cell_idx_ = cell;
                            hover_px_x_ = px;
                            hover_px_y_ = py;
                        }
                    }
                }
            } else if (mode_ == ComparisonMode::Difference && tex_diff &&
                       tex_diff_w > 0 && tex_diff_h > 0) {
                ImVec2 pos, size;
                compute_image_rect(tex_diff_w, tex_diff_h, pos, size);
                if (size.x > 0.0f && size.y > 0.0f) {
                    float scale = size.x / tex_diff_w;
                    int px = static_cast<int>((mp.x - pos.x) / scale);
                    int py = static_cast<int>((mp.y - pos.y) / scale);
                    if (px >= 0 && px < tex_diff_w &&
                        py >= 0 && py < tex_diff_h) {
                        hover_valid_ = true;
                        hover_cell_idx_ = 0;
                        hover_px_x_ = px;
                        hover_px_y_ = py;
                    }
                }
            }
        }
    }

    draw_selection_rect();

    dl->PopClipRect();

    ImGui::Dummy(avail);
}

void Viewport::render_split(const std::vector<SDL_Texture*>& tex_ptrs,
                             const std::vector<int>& tex_ws,
                             const std::vector<int>& tex_hs,
                             const std::vector<const char*>& labels) {
    int n = static_cast<int>(tex_ptrs.size());
    if (n == 0) return;

    ImDrawList* dl = ImGui::GetWindowDrawList();

    int cols, rows;
    if (n == 1) { cols = 1; rows = 1; }
    else if (n == 2) { cols = 2; rows = 1; }
    else if (n <= 4) { cols = 2; rows = 2; }
    else if (n <= 6) { cols = 3; rows = 2; }
    else { cols = 3; rows = (n + cols - 1) / cols; }

    split_cols_ = cols;
    split_rows_ = rows;

    float cell_w = vp_size_.x / cols;
    float cell_h = vp_size_.y / rows;

    for (int i = 0; i < n; i++) {
        int col = i % cols;
        int row = i / cols;

        float cell_x = vp_origin_.x + col * cell_w;
        float cell_y = vp_origin_.y + row * cell_h;

        if (!tex_ptrs[i]) {
            ImVec2 text_size = ImGui::CalcTextSize("Empty");
            dl->AddText(ImVec2(cell_x + (cell_w - text_size.x) * 0.5f,
                               cell_y + (cell_h - text_size.y) * 0.5f),
                        IM_COL32(255, 255, 255, 80), "Empty");
            continue;
        }

        float fit_scale = std::min(cell_w / tex_ws[i], cell_h / tex_hs[i]);
        float scale = fit_scale * zoom_;
        float disp_w = tex_ws[i] * scale;
        float disp_h = tex_hs[i] * scale;

        float img_x = cell_x + (cell_w - disp_w) * 0.5f + pan_x_;
        float img_y = cell_y + (cell_h - disp_h) * 0.5f + pan_y_;

        ImVec2 img_min(img_x, img_y);
        ImVec2 img_max(img_x + disp_w, img_y + disp_h);

        dl->PushClipRect(ImVec2(cell_x, cell_y),
                         ImVec2(cell_x + cell_w, cell_y + cell_h), true);
        dl->AddImage(to_tex_id(tex_ptrs[i]), img_min, img_max);

        if (show_grid_ && scale > 0.0f) {
            draw_grid(img_min, ImVec2(disp_w, disp_h), tex_ws[i], tex_hs[i], scale);
        }

        if (labels[i]) {
            draw_image_label(labels[i], ImVec2(cell_x, cell_y), ImVec2(cell_w, cell_h));
        }
        dl->PopClipRect();

        // Rulers are drawn outside the clip rect (above/left of image)
        if (show_ruler_ && scale > 0.0f) {
            dl->PushClipRect(ImVec2(cell_x, cell_y - 20),
                             ImVec2(cell_x + cell_w, cell_y + cell_h), true);
            draw_ruler(img_min, ImVec2(disp_w, disp_h), tex_ws[i], tex_hs[i], scale);
            dl->PopClipRect();
        }
    }

    // Draw cell dividers
    ImU32 divider_col = IM_COL32(255, 255, 255, 40);
    for (int c = 1; c < cols; c++) {
        float x = vp_origin_.x + c * cell_w;
        dl->AddLine(ImVec2(x, vp_origin_.y),
                    ImVec2(x, vp_origin_.y + vp_size_.y), divider_col);
    }
    for (int r = 1; r < rows; r++) {
        float y = vp_origin_.y + r * cell_h;
        dl->AddLine(ImVec2(vp_origin_.x, y),
                    ImVec2(vp_origin_.x + vp_size_.x, y), divider_col);
    }
}

void Viewport::render_overlay(const std::vector<SDL_Texture*>& tex_ptrs,
                               const std::vector<int>& tex_ws,
                               const std::vector<int>& tex_hs,
                               const std::vector<const char*>& labels) {
    int n = static_cast<int>(tex_ptrs.size());
    if (n == 0) return;

    ImDrawList* dl = ImGui::GetWindowDrawList();

    if (n == 1 || !tex_ptrs[0]) {
        if (tex_ptrs[0]) {
            ImVec2 pos, size;
            compute_image_rect(tex_ws[0], tex_hs[0], pos, size);
            dl->AddImage(to_tex_id(tex_ptrs[0]), pos,
                         ImVec2(pos.x + size.x, pos.y + size.y));
            float scale = size.x / tex_ws[0];
            if (show_grid_) draw_grid(pos, size, tex_ws[0], tex_hs[0], scale);
            if (show_ruler_) draw_ruler(pos, size, tex_ws[0], tex_hs[0], scale);
            if (labels[0]) draw_image_label(labels[0], vp_origin_, vp_size_);
        } else {
            dl->AddText(ImVec2(vp_origin_.x + vp_size_.x * 0.5f - 60,
                               vp_origin_.y + vp_size_.y * 0.5f),
                        IM_COL32(255, 255, 255, 80), "Select images for overlay");
        }
        return;
    }

    if (!tex_ptrs[1]) {
        ImVec2 pos, size;
        compute_image_rect(tex_ws[0], tex_hs[0], pos, size);
        dl->AddImage(to_tex_id(tex_ptrs[0]), pos,
                     ImVec2(pos.x + size.x, pos.y + size.y));
        float scale = size.x / tex_ws[0];
        if (show_grid_) draw_grid(pos, size, tex_ws[0], tex_hs[0], scale);
        if (show_ruler_) draw_ruler(pos, size, tex_ws[0], tex_hs[0], scale);
        if (labels[0]) draw_image_label(labels[0], vp_origin_, vp_size_);
        return;
    }

    // Two-image A/B slider overlay
    int img_w = std::max(tex_ws[0], tex_ws[1]);
    int img_h = std::max(tex_hs[0], tex_hs[1]);

    ImVec2 pos, size;
    compute_image_rect(img_w, img_h, pos, size);

    float line_x = vp_origin_.x + vp_size_.x * slider_pos_;

    float uv_a_scale_x = static_cast<float>(tex_ws[0]) / img_w;
    float uv_b_scale_x = static_cast<float>(tex_ws[1]) / img_w;

    // Draw image B (right side)
    {
        dl->PushClipRect(ImVec2(line_x, vp_origin_.y),
                         ImVec2(vp_origin_.x + vp_size_.x, vp_origin_.y + vp_size_.y), true);
        ImVec2 uv0(0, 0);
        ImVec2 uv1(uv_b_scale_x, 1);
        dl->AddImage(to_tex_id(tex_ptrs[1]), pos,
                     ImVec2(pos.x + size.x, pos.y + size.y), uv0, uv1);
        dl->PopClipRect();
    }

    // Draw image A (left side)
    {
        dl->PushClipRect(vp_origin_, ImVec2(line_x, vp_origin_.y + vp_size_.y), true);
        ImVec2 uv0(0, 0);
        ImVec2 uv1(uv_a_scale_x, 1);
        dl->AddImage(to_tex_id(tex_ptrs[0]), pos,
                     ImVec2(pos.x + size.x, pos.y + size.y), uv0, uv1);
        dl->PopClipRect();
    }

    // Grid overlay (drawn on the composite image)
    if (show_grid_ && size.x > 0.0f) {
        float scale = size.x / img_w;
        draw_grid(pos, size, img_w, img_h, scale);
    }

    // Ruler
    if (show_ruler_ && size.x > 0.0f) {
        float scale = size.x / img_w;
        draw_ruler(pos, size, img_w, img_h, scale);
    }

    // Slider line
    {
        float vp_top = vp_origin_.y;
        float vp_bot = vp_origin_.y + vp_size_.y;
        dl->AddLine(ImVec2(line_x, vp_top),
                    ImVec2(line_x, vp_bot),
                    IM_COL32(255, 255, 255, 220), 2.0f);

        float handle_sz = 8.0f;
        dl->AddTriangleFilled(
            ImVec2(line_x - handle_sz, vp_top),
            ImVec2(line_x + handle_sz, vp_top),
            ImVec2(line_x, vp_top + handle_sz * 1.5f),
            IM_COL32(255, 255, 255, 220));
        dl->AddTriangleFilled(
            ImVec2(line_x - handle_sz, vp_bot),
            ImVec2(line_x + handle_sz, vp_bot),
            ImVec2(line_x, vp_bot - handle_sz * 1.5f),
            IM_COL32(255, 255, 255, 220));
    }

    // Slider interaction
    {
        constexpr float slider_hit_radius = 12.0f;
        float line_x = vp_origin_.x + vp_size_.x * slider_pos_;

        ImVec2 strip_pos(vp_origin_.x + std::max(0.0f, line_x - slider_hit_radius - vp_origin_.x),
                         vp_origin_.y);
        float strip_x_local = line_x - vp_origin_.x - slider_hit_radius;
        if (strip_x_local < 0) strip_x_local = 0;
        float strip_w = slider_hit_radius * 2.0f;
        if (strip_x_local + strip_w > vp_size_.x)
            strip_w = vp_size_.x - strip_x_local;

        ImGui::SetCursorScreenPos(ImVec2(vp_origin_.x + strip_x_local, vp_origin_.y));
        ImGui::InvisibleButton("##overlay_slider", ImVec2(strip_w, vp_size_.y));
        slider_dragging_ = ImGui::IsItemActive();
        if (slider_dragging_ || ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }
        if (slider_dragging_) {
            float mouse_x = ImGui::GetIO().MousePos.x - vp_origin_.x;
            slider_pos_ = std::clamp(mouse_x / vp_size_.x, 0.0f, 1.0f);
        }
    }

    // Labels
    if (labels[0]) {
        draw_image_label(labels[0], vp_origin_, vp_size_);
    }
    if (labels[1]) {
        float slider_screen_x = vp_origin_.x + vp_size_.x * slider_pos_;
        ImVec2 b_anchor(slider_screen_x, vp_origin_.y);
        draw_image_label(labels[1], b_anchor, ImVec2(vp_size_.x - slider_screen_x, vp_size_.y));
    }
}

void Viewport::render_difference(SDL_Texture* tex_diff, int tex_diff_w, int tex_diff_h,
                                  const std::vector<const char*>& labels) {
    if (!tex_diff) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddText(ImVec2(vp_origin_.x + vp_size_.x * 0.5f - 80,
                           vp_origin_.y + vp_size_.y * 0.5f - 10),
                    IM_COL32(255, 255, 255, 80), "No difference map available");
        dl->AddText(ImVec2(vp_origin_.x + vp_size_.x * 0.5f - 110,
                           vp_origin_.y + vp_size_.y * 0.5f + 10),
                    IM_COL32(255, 255, 255, 80), "Select exactly 2 images to compute diff");
        return;
    }

    ImVec2 pos, size;
    compute_image_rect(tex_diff_w, tex_diff_h, pos, size);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddImage(to_tex_id(tex_diff), pos, ImVec2(pos.x + size.x, pos.y + size.y));

    if (show_grid_ && size.x > 0.0f) {
        float scale = size.x / tex_diff_w;
        draw_grid(pos, size, tex_diff_w, tex_diff_h, scale);
    }

    if (show_ruler_ && size.x > 0.0f) {
        float scale = size.x / tex_diff_w;
        draw_ruler(pos, size, tex_diff_w, tex_diff_h, scale);
    }

    if (labels.size() >= 2 && labels[0] && labels[1]) {
        std::string diff_label = std::string("Diff: ") + labels[0] + " vs " + labels[1];
        draw_image_label(diff_label.c_str(), vp_origin_, vp_size_);
    }
}

void Viewport::cell_at(ImVec2 screen_pt, ImVec2& out_origin, ImVec2& out_size) const {
    float cell_w = vp_size_.x / split_cols_;
    float cell_h = vp_size_.y / split_rows_;

    int col = static_cast<int>((screen_pt.x - vp_origin_.x) / cell_w);
    int row = static_cast<int>((screen_pt.y - vp_origin_.y) / cell_h);
    col = std::clamp(col, 0, split_cols_ - 1);
    row = std::clamp(row, 0, split_rows_ - 1);

    out_origin = ImVec2(vp_origin_.x + col * cell_w,
                        vp_origin_.y + row * cell_h);
    out_size = ImVec2(cell_w, cell_h);
}

} // namespace idiff
