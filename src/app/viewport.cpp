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

    // anchor is in screen coords.  Convert to content-relative coords,
    // then adjust pan so that point stays under the cursor.
    //
    // Before zoom:  screen_x = vp_origin_.x + vp_size_.x/2 + pan_x_ + content_x * fit_scale * zoom_
    // After  zoom:  screen_x = vp_origin_.x + vp_size_.x/2 + pan_x'  + content_x * fit_scale * new_zoom
    // We want screen_x unchanged  =>  pan_x' = pan_x + content_x * fit_scale * (zoom_ - new_zoom)
    //
    // content_x = (anchor.x - vp_origin_.x - vp_size_.x/2 - pan_x_) / (fit_scale * zoom_)
    //
    // Substituting:
    //   pan_x' = pan_x + (anchor.x - cx) * (1 - new_zoom / zoom_)
    //   where cx = vp_origin_.x + vp_size_.x/2 + pan_x_

    float cx = vp_origin_.x + vp_size_.x * 0.5f + pan_x_;
    float cy = vp_origin_.y + vp_size_.y * 0.5f + pan_y_;

    float ratio = 1.0f - new_zoom / zoom_;
    pan_x_ += (anchor.x - cx) * ratio;
    pan_y_ += (anchor.y - cy) * ratio;

    zoom_ = new_zoom;
}

void Viewport::zoom_to_rect(ImVec2 rect_min, ImVec2 rect_max) {
    float rw = rect_max.x - rect_min.x;
    float rh = rect_max.y - rect_min.y;
    if (rw < 4.0f || rh < 4.0f || vp_size_.x < 1.0f || vp_size_.y < 1.0f) return;

    // Center of the selection in screen coords
    float sel_cx = (rect_min.x + rect_max.x) * 0.5f;
    float sel_cy = (rect_min.y + rect_max.y) * 0.5f;

    // Center of the viewport in screen coords
    float vp_cx = vp_origin_.x + vp_size_.x * 0.5f;
    float vp_cy = vp_origin_.y + vp_size_.y * 0.5f;

    // How much to scale up so the selection fills the viewport
    float scale_factor = std::min(vp_size_.x / rw, vp_size_.y / rh);

    // New pan: the selection center should map to the viewport center.
    // Before: sel_cx = vp_cx + pan_x_ + offset_in_content
    // After:  vp_cx  = vp_cx + pan_x' + offset_in_content * scale_factor
    // => pan_x' = (pan_x_ + (sel_cx - vp_cx)) * scale_factor - (sel_cx - vp_cx) * scale_factor
    //           = (pan_x_ - (sel_cx - vp_cx)) * scale_factor + ... simplify:
    //
    // Actually simpler: after zoom, we want the content point that was at sel_cx
    // to be at vp_cx.
    //   content_x = (sel_cx - vp_cx - pan_x_) / zoom_  (in fit-scaled units)
    //   new_zoom = zoom_ * scale_factor
    //   vp_cx = vp_cx + new_pan_x + content_x * new_zoom
    //   new_pan_x = -content_x * new_zoom = -(sel_cx - vp_cx - pan_x_) / zoom_ * new_zoom
    //             = -(sel_cx - vp_cx - pan_x_) * scale_factor

    float new_zoom = std::clamp(zoom_ * scale_factor, 0.05f, 64.0f);
    float actual_factor = new_zoom / zoom_;

    pan_x_ = -(sel_cx - vp_cx - pan_x_) * actual_factor;
    pan_y_ = -(sel_cy - vp_cy - pan_y_) * actual_factor;
    zoom_ = new_zoom;
}

void Viewport::fit_to_content() {
    zoom_ = 1.0f;
    pan_x_ = 0.0f;
    pan_y_ = 0.0f;
}

void Viewport::zoom_to_actual() {
    // 1:1 pixel mapping: the fit_scale in render is min(vp/img),
    // so actual pixels means zoom = 1/fit_scale = max(img/vp).
    // But we don't know the image size here; we stored content_w_/h_ during render.
    if (content_w_ <= 0 || content_h_ <= 0 || vp_size_.x <= 0 || vp_size_.y <= 0) {
        zoom_ = 1.0f;
        pan_x_ = 0.0f;
        pan_y_ = 0.0f;
        return;
    }

    float fit_scale = std::min(vp_size_.x / content_w_, vp_size_.y / content_h_);
    float new_zoom = 1.0f / fit_scale;
    // Center the image
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

void Viewport::draw_image_label(const char* label, ImVec2 img_pos, ImVec2 img_size) {
    ImVec2 text_size = ImGui::CalcTextSize(label);
    float pad_x = 6.0f, pad_y = 4.0f;

    ImVec2 rect_min(img_pos.x + 4, img_pos.y + 4);
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
            if (labels[0]) draw_image_label(labels[0], pos, size);
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
        if (labels[0]) draw_image_label(labels[0], pos, size);
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

    // Labels
    if (labels[0]) {
        draw_image_label(labels[0], pos, ImVec2(split_x, size.y));
    }
    if (labels[1]) {
        ImVec2 b_pos(pos.x + split_x, pos.y);
        draw_image_label(labels[1], b_pos, ImVec2(size.x - split_x, size.y));
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
        draw_image_label(diff_label.c_str(), pos, size);
    }
}

} // namespace idiff
