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

    // Use the cell that contains the anchor point as the reference frame.
    // In non-split modes cell == viewport.
    ImVec2 cell_org, cell_sz;
    cell_at(anchor, cell_org, cell_sz);

    // cx/cy = center of the cell + current pan offset.
    // The image in each cell is rendered at:
    //   img_x = cell_org.x + (cell_sz.x - disp_w)/2 + pan_x_
    // So the "neutral center" for pan is cell_org + cell_sz/2.
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

    // Use the cell that contains the selection center as the reference frame.
    ImVec2 sel_center((rect_min.x + rect_max.x) * 0.5f,
                      (rect_min.y + rect_max.y) * 0.5f);
    ImVec2 cell_org, cell_sz;
    cell_at(sel_center, cell_org, cell_sz);

    float sel_cx = sel_center.x;
    float sel_cy = sel_center.y;

    // Center of the cell (the neutral pan origin for this cell)
    float cell_cx = cell_org.x + cell_sz.x * 0.5f;
    float cell_cy = cell_org.y + cell_sz.y * 0.5f;

    // Scale so the selection fills the cell (not the whole viewport)
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
    // 1:1 pixel mapping: the fit_scale in render is min(area/img),
    // so actual pixels means zoom = 1/fit_scale.
    if (content_w_ <= 0 || content_h_ <= 0 || vp_size_.x <= 0 || vp_size_.y <= 0) {
        zoom_ = 1.0f;
        pan_x_ = 0.0f;
        pan_y_ = 0.0f;
        return;
    }

    // In split mode, fit_scale is based on cell size, not viewport size.
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

    // Label is pinned to the anchor area (viewport / cell top-left),
    // so it stays visible regardless of zoom or pan.
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
    // fit_scale: scale so the image fits the viewport at zoom=1
    float fit_scale = std::min(vp_size_.x / img_w, vp_size_.y / img_h);
    float scale = fit_scale * zoom_;

    float disp_w = img_w * scale;
    float disp_h = img_h * scale;

    // Center in viewport, then apply pan
    float x = vp_origin_.x + (vp_size_.x - disp_w) * 0.5f + pan_x_;
    float y = vp_origin_.y + (vp_size_.y - disp_h) * 0.5f + pan_y_;

    out_pos = ImVec2(x, y);
    out_size = ImVec2(disp_w, disp_h);
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

    if (avail.x < 10 || avail.y < 10) return;

    // Track content dimensions for zoom_to_actual
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

    // Clip rendering to the viewport area
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(vp_origin_, ImVec2(vp_origin_.x + vp_size_.x,
                                         vp_origin_.y + vp_size_.y), true);

    // Reset split layout for non-split modes (render_split will set them)
    if (mode_ != ComparisonMode::Split) {
        split_cols_ = 1;
        split_rows_ = 1;
    }

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

    draw_selection_rect();

    dl->PopClipRect();

    // Reserve the space so ImGui layout knows we used it
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

    // Record layout so zoom helpers can use cell dimensions
    split_cols_ = cols;
    split_rows_ = rows;

    float cell_w = vp_size_.x / cols;
    float cell_h = vp_size_.y / rows;

    for (int i = 0; i < n; i++) {
        int col = i % cols;
        int row = i / cols;

        // Cell origin in screen coords
        float cell_x = vp_origin_.x + col * cell_w;
        float cell_y = vp_origin_.y + row * cell_h;

        if (!tex_ptrs[i]) {
            // Draw "Empty" text centered in cell
            ImVec2 text_size = ImGui::CalcTextSize("Empty");
            dl->AddText(ImVec2(cell_x + (cell_w - text_size.x) * 0.5f,
                               cell_y + (cell_h - text_size.y) * 0.5f),
                        IM_COL32(255, 255, 255, 80), "Empty");
            continue;
        }

        // Fit image into cell, then apply zoom and pan
        float fit_scale = std::min(cell_w / tex_ws[i], cell_h / tex_hs[i]);
        float scale = fit_scale * zoom_;
        float disp_w = tex_ws[i] * scale;
        float disp_h = tex_hs[i] * scale;

        float img_x = cell_x + (cell_w - disp_w) * 0.5f + pan_x_;
        float img_y = cell_y + (cell_h - disp_h) * 0.5f + pan_y_;

        ImVec2 img_min(img_x, img_y);
        ImVec2 img_max(img_x + disp_w, img_y + disp_h);

        // Clip to cell
        dl->PushClipRect(ImVec2(cell_x, cell_y),
                         ImVec2(cell_x + cell_w, cell_y + cell_h), true);
        dl->AddImage(to_tex_id(tex_ptrs[i]), img_min, img_max);

        if (labels[i]) {
            draw_image_label(labels[i], ImVec2(cell_x, cell_y), ImVec2(cell_w, cell_h));
        }
        dl->PopClipRect();
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
        if (labels[0]) draw_image_label(labels[0], vp_origin_, vp_size_);
        return;
    }

    // Two-image A/B slider overlay
    int img_w = std::max(tex_ws[0], tex_ws[1]);
    int img_h = std::max(tex_hs[0], tex_hs[1]);

    ImVec2 pos, size;
    compute_image_rect(img_w, img_h, pos, size);

    float split_x = size.x * slider_pos_;

    // UV scale for images that may be smaller than the composite
    float uv_a_scale_x = static_cast<float>(tex_ws[0]) / img_w;
    float uv_b_scale_x = static_cast<float>(tex_ws[1]) / img_w;

    // Draw image B (right side of slider)
    {
        float u0 = (split_x / size.x) * uv_b_scale_x;
        float u1 = 1.0f * uv_b_scale_x;
        ImVec2 rect_min(pos.x + split_x, pos.y);
        ImVec2 rect_max(pos.x + size.x, pos.y + size.y);
        dl->AddImage(to_tex_id(tex_ptrs[1]), rect_min, rect_max,
                     ImVec2(u0, 0), ImVec2(u1, 1));
    }

    // Draw image A (left side of slider)
    {
        float u1 = (split_x / size.x) * uv_a_scale_x;
        ImVec2 rect_min(pos.x, pos.y);
        ImVec2 rect_max(pos.x + split_x, pos.y + size.y);
        dl->AddImage(to_tex_id(tex_ptrs[0]), rect_min, rect_max,
                     ImVec2(0, 0), ImVec2(u1, 1));
    }

    // Slider line
    {
        float line_x = pos.x + split_x;
        dl->AddLine(ImVec2(line_x, pos.y),
                    ImVec2(line_x, pos.y + size.y),
                    IM_COL32(255, 255, 255, 220), 2.0f);

        float handle_sz = 8.0f;
        dl->AddTriangleFilled(
            ImVec2(line_x - handle_sz, pos.y),
            ImVec2(line_x + handle_sz, pos.y),
            ImVec2(line_x, pos.y + handle_sz * 1.5f),
            IM_COL32(255, 255, 255, 220));
        dl->AddTriangleFilled(
            ImVec2(line_x - handle_sz, pos.y + size.y),
            ImVec2(line_x + handle_sz, pos.y + size.y),
            ImVec2(line_x, pos.y + size.y - handle_sz * 1.5f),
            IM_COL32(255, 255, 255, 220));
    }

    // Handle slider dragging — use an invisible button over the image area
    {
        ImGui::SetCursorScreenPos(pos);
        ImGui::InvisibleButton("##overlay_slider", size);
        if (ImGui::IsItemActive() || ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }
        if (ImGui::IsItemActive()) {
            float mouse_x = ImGui::GetIO().MousePos.x - pos.x;
            slider_pos_ = std::clamp(mouse_x / size.x, 0.0f, 1.0f);
        }
    }

    // Labels — anchored to viewport edges, not to the (zoomed/panned) image
    if (labels[0]) {
        draw_image_label(labels[0], vp_origin_, vp_size_);
    }
    if (labels[1]) {
        // Right-side label anchored at the slider position within the viewport
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

    if (labels.size() >= 2 && labels[0] && labels[1]) {
        std::string diff_label = std::string("Diff: ") + labels[0] + " vs " + labels[1];
        draw_image_label(diff_label.c_str(), vp_origin_, vp_size_);
    }
}

void Viewport::cell_at(ImVec2 screen_pt, ImVec2& out_origin, ImVec2& out_size) const {
    float cell_w = vp_size_.x / split_cols_;
    float cell_h = vp_size_.y / split_rows_;

    // Determine which cell the point falls in
    int col = static_cast<int>((screen_pt.x - vp_origin_.x) / cell_w);
    int row = static_cast<int>((screen_pt.y - vp_origin_.y) / cell_h);
    col = std::clamp(col, 0, split_cols_ - 1);
    row = std::clamp(row, 0, split_rows_ - 1);

    out_origin = ImVec2(vp_origin_.x + col * cell_w,
                        vp_origin_.y + row * cell_h);
    out_size = ImVec2(cell_w, cell_h);
}

} // namespace idiff
