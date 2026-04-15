#include "app/viewport.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace idiff {

Viewport::Viewport() = default;

ImTextureID Viewport::to_tex_id(SDL_Texture* tex) {
    return (ImTextureID)(uintptr_t)tex;
}

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

void Viewport::render(const std::vector<SDL_Texture*>& tex_ptrs,
                      const std::vector<int>& tex_ws,
                      const std::vector<int>& tex_hs,
                      const std::vector<const char*>& labels,
                      SDL_Texture* tex_diff, int tex_diff_w, int tex_diff_h) {
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 10 || avail.y < 10) return;

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
}

void Viewport::render_split(const std::vector<SDL_Texture*>& tex_ptrs,
                             const std::vector<int>& tex_ws,
                             const std::vector<int>& tex_hs,
                             const std::vector<const char*>& labels) {
    int n = static_cast<int>(tex_ptrs.size());
    if (n == 0) return;

    ImVec2 avail = ImGui::GetContentRegionAvail();

    int cols, rows;
    if (n == 1) { cols = 1; rows = 1; }
    else if (n == 2) { cols = 2; rows = 1; }
    else if (n <= 4) { cols = 2; rows = 2; }
    else if (n <= 6) { cols = 3; rows = 2; }
    else { cols = 3; rows = (n + cols - 1) / cols; }

    float cell_w = avail.x / cols;
    float cell_h = avail.y / rows;

    for (int i = 0; i < n; i++) {
        int col = i % cols;
        int row = i / cols;

        if (col > 0) ImGui::SameLine(col * cell_w);

        ImGui::BeginGroup();
        ImGui::PushID(i);

        ImVec2 group_pos = ImGui::GetCursorScreenPos();

        if (tex_ptrs[i]) {
            float scale = std::min(cell_w / tex_ws[i], cell_h / tex_hs[i]) * zoom_;
            ImVec2 img_size(tex_ws[i] * scale, tex_hs[i] * scale);

            float off_x = (cell_w - img_size.x) * 0.5f;
            float off_y = (cell_h - img_size.y) * 0.5f;
            if (off_x > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + off_x);
            if (off_y > 0) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + off_y);

            ImVec2 img_screen_pos = ImGui::GetCursorScreenPos();
            ImGui::Image(to_tex_id(tex_ptrs[i]), img_size);

            if (labels[i]) {
                draw_image_label(labels[i], img_screen_pos, img_size);
            }
        } else {
            ImVec2 center(cell_w * 0.5f, cell_h * 0.5f);
            ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + center.x - 40,
                                        ImGui::GetCursorPosY() + center.y - 8));
            ImGui::TextDisabled("Empty");
        }

        ImGui::PopID();
        ImGui::EndGroup();
    }
}

void Viewport::render_overlay(const std::vector<SDL_Texture*>& tex_ptrs,
                               const std::vector<int>& tex_ws,
                               const std::vector<int>& tex_hs,
                               const std::vector<const char*>& labels) {
    int n = static_cast<int>(tex_ptrs.size());
    if (n == 0) return;

    ImVec2 avail = ImGui::GetContentRegionAvail();

    if (n == 1 || !tex_ptrs[0]) {
        if (tex_ptrs[0]) {
            float scale = std::min(avail.x / tex_ws[0], avail.y / tex_hs[0]) * zoom_;
            ImVec2 img_size(tex_ws[0] * scale, tex_hs[0] * scale);
            ImVec2 img_screen_pos = ImGui::GetCursorScreenPos();
            ImGui::Image(to_tex_id(tex_ptrs[0]), img_size);
            if (labels[0]) draw_image_label(labels[0], img_screen_pos, img_size);
        } else {
            ImGui::TextDisabled("Select images for overlay");
        }
        return;
    }

    if (n < 2 || !tex_ptrs[1]) {
        float scale = std::min(avail.x / tex_ws[0], avail.y / tex_hs[0]) * zoom_;
        ImVec2 img_size(tex_ws[0] * scale, tex_hs[0] * scale);
        ImVec2 img_screen_pos = ImGui::GetCursorScreenPos();
        ImGui::Image(to_tex_id(tex_ptrs[0]), img_size);
        if (labels[0]) draw_image_label(labels[0], img_screen_pos, img_size);
        return;
    }

    // Two-image A/B slider overlay using ImDrawList clip-rect rendering
    // Both textures should be the same size after upscaling, but handle mismatches.
    int img_w = std::max(tex_ws[0], tex_ws[1]);
    int img_h = std::max(tex_hs[0], tex_hs[1]);

    float scale = std::min(avail.x / static_cast<float>(img_w),
                           avail.y / static_cast<float>(img_h)) * zoom_;
    ImVec2 img_size(img_w * scale, img_h * scale);

    ImVec2 img_screen_pos = ImGui::GetCursorScreenPos();

    // Reserve space
    ImGui::Dummy(img_size);

    float split_x = img_size.x * slider_pos_;

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Compute UV ranges for each image (handle different texture sizes)
    float uv_b_x0 = (tex_ws[1] > 0) ? (split_x / img_size.x) : 0.0f;
    float uv_a_x1 = (tex_ws[0] > 0) ? (split_x / img_size.x) : 0.0f;

    // Scale UV by texture-to-display ratio for images that aren't full-width
    float uv_a_scale_x = static_cast<float>(tex_ws[0]) / img_w;
    float uv_b_scale_x = static_cast<float>(tex_ws[1]) / img_w;

    // Draw image B (right side of slider)
    {
        float u0 = uv_b_x0 * uv_b_scale_x;
        float u1 = 1.0f * uv_b_scale_x;
        ImVec2 uv_b0(u0, 0.0f);
        ImVec2 uv_b1(u1, 1.0f);
        ImVec2 rect_min(img_screen_pos.x + split_x, img_screen_pos.y);
        ImVec2 rect_max(img_screen_pos.x + img_size.x, img_screen_pos.y + img_size.y);
        dl->AddImage(to_tex_id(tex_ptrs[1]), rect_min, rect_max, uv_b0, uv_b1);
    }

    // Draw image A (left side of slider)
    {
        float u1 = uv_a_x1 * uv_a_scale_x;
        ImVec2 uv_a0(0.0f, 0.0f);
        ImVec2 uv_a1(u1, 1.0f);
        ImVec2 rect_min(img_screen_pos.x, img_screen_pos.y);
        ImVec2 rect_max(img_screen_pos.x + split_x, img_screen_pos.y + img_size.y);
        dl->AddImage(to_tex_id(tex_ptrs[0]), rect_min, rect_max, uv_a0, uv_a1);
    }

    // Slider line
    {
        float line_x = img_screen_pos.x + split_x;
        dl->AddLine(ImVec2(line_x, img_screen_pos.y),
                     ImVec2(line_x, img_screen_pos.y + img_size.y),
                     IM_COL32(255, 255, 255, 220), 2.0f);

        float handle_sz = 8.0f;
        dl->AddTriangleFilled(
            ImVec2(line_x - handle_sz, img_screen_pos.y),
            ImVec2(line_x + handle_sz, img_screen_pos.y),
            ImVec2(line_x, img_screen_pos.y + handle_sz * 1.5f),
            IM_COL32(255, 255, 255, 220));
        dl->AddTriangleFilled(
            ImVec2(line_x - handle_sz, img_screen_pos.y + img_size.y),
            ImVec2(line_x + handle_sz, img_screen_pos.y + img_size.y),
            ImVec2(line_x, img_screen_pos.y + img_size.y - handle_sz * 1.5f),
            IM_COL32(255, 255, 255, 220));
    }

    // Handle slider dragging
    {
        ImGui::SetCursorScreenPos(img_screen_pos);
        ImGui::InvisibleButton("##overlay_slider", img_size);
        if (ImGui::IsItemActive() || ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }
        if (ImGui::IsItemActive()) {
            float mouse_x = ImGui::GetIO().MousePos.x - img_screen_pos.x;
            slider_pos_ = std::clamp(mouse_x / img_size.x, 0.0f, 1.0f);
        }
    }

    // Labels
    if (labels[0]) {
        draw_image_label(labels[0], img_screen_pos, ImVec2(split_x, img_size.y));
    }
    if (labels[1]) {
        ImVec2 b_pos(img_screen_pos.x + split_x, img_screen_pos.y);
        draw_image_label(labels[1], b_pos, ImVec2(img_size.x - split_x, img_size.y));
    }
}

void Viewport::render_difference(SDL_Texture* tex_diff, int tex_diff_w, int tex_diff_h,
                                  const std::vector<const char*>& labels) {
    if (!tex_diff) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImVec2 center(avail.x * 0.5f, avail.y * 0.5f);
        ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + center.x - 100,
                                    ImGui::GetCursorPosY() + center.y - 20));
        ImGui::TextDisabled("No difference map available");
        ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + center.x - 130,
                                    ImGui::GetCursorPosY() + 8));
        ImGui::TextDisabled("Select exactly 2 images to compute diff");
        return;
    }

    ImVec2 avail = ImGui::GetContentRegionAvail();
    float scale = std::min(avail.x / tex_diff_w, avail.y / tex_diff_h) * zoom_;
    ImVec2 img_size(tex_diff_w * scale, tex_diff_h * scale);

    ImVec2 img_screen_pos = ImGui::GetCursorScreenPos();
    ImGui::Image(to_tex_id(tex_diff), img_size);

    if (labels.size() >= 2 && labels[0] && labels[1]) {
        std::string diff_label = std::string("Diff: ") + labels[0] + " vs " + labels[1];
        draw_image_label(diff_label.c_str(), img_screen_pos, img_size);
    }
}

} // namespace idiff
