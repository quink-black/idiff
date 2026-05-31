#include "app/properties_panel.h"

#include <imgui.h>

#include <cmath>
#include <cstdio>

#include "app/viewport.h"
#include "core/image.h"

namespace idiff {

PropertiesPanel::PropertiesPanel() = default;
PropertiesPanel::~PropertiesPanel() = default;

void PropertiesPanel::render(const Image* image_a, const Image* image_b,
                             const Image* display_a, const Image* display_b,
                             const char* name_a, const char* name_b) {
    ImGui::SetNextWindowSize(ImVec2(300, 400), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Properties")) {
        ImGui::End();
        return;
    }
    render_inline(image_a, image_b, display_a, display_b, name_a, name_b);
    ImGui::End();
}

void PropertiesPanel::render_inline(const Image* image_a, const Image* image_b,
                                     const Image* display_a, const Image* display_b,
                                     const char* name_a, const char* name_b) {
    render_image_props("A", name_a, image_a, display_a);
    if (image_a && image_b) {
        ImGui::Separator();
    }
    render_image_props("B", name_b, image_b, display_b);
}

void PropertiesPanel::render_image_props(const char* slot_label, const char* name,
                                          const Image* img, const Image* display_img) {
    if (!img) {
        ImGui::TextDisabled("%s: No image", slot_label);
        return;
    }

    // Header shows "A | filename" / "B | filename" so the user can tell which
    // selected image is used as A / B in overlay and diff modes.
    //
    // NOTE: use an ASCII separator here.  The default ImGui font only covers
    // Basic Latin, so Unicode separators like em-dash (U+2014) render as the
    // fallback '?' glyph.
    char header[512];
    if (name && name[0]) {
        std::snprintf(header, sizeof(header), "%s | %s", slot_label, name);
    } else {
        std::snprintf(header, sizeof(header), "Image %s", slot_label);
    }

    // Give the header a stable ID so its open/close state is not tied to the filename.
    ImGui::PushID(slot_label);
    bool open = ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::PopID();
    if (open) {
        const auto& info = img->info();

        if (ImGui::BeginTable("##props_table", 2, ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted("Resolution");
            ImGui::TableNextColumn(); ImGui::Text("%d x %d", info.width, info.height);

            // Show display (upscaled) resolution if different from original
            if (display_img && display_img != img) {
                const auto& disp_info = display_img->info();
                if (disp_info.width != info.width || disp_info.height != info.height) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextUnformatted("Display");
                    ImGui::TableNextColumn(); ImGui::Text("%d x %d", disp_info.width, disp_info.height);
                }
            }

            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted("Bit depth");
            ImGui::TableNextColumn();
            if (info.source_bit_depth > 0 && info.source_bit_depth != info.bit_depth)
                ImGui::Text("%d (%d-bit source)", info.bit_depth, info.source_bit_depth);
            else
                ImGui::Text("%d", info.bit_depth);

            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted("Has alpha");
            ImGui::TableNextColumn(); ImGui::Text("%s", info.has_alpha ? "Yes" : "No");

            const char* fmt_name = "Unknown";
            switch (info.source_format) {
                case SourceFormat::PNG:    fmt_name = "PNG"; break;
                case SourceFormat::JPEG:   fmt_name = "JPEG"; break;
                case SourceFormat::WebP:   fmt_name = "WebP"; break;
                case SourceFormat::TIFF:   fmt_name = "TIFF"; break;
                case SourceFormat::BMP:    fmt_name = "BMP"; break;
                case SourceFormat::RAW:    fmt_name = "RAW"; break;
                case SourceFormat::HEIF:   fmt_name = "HEIF"; break;
                case SourceFormat::AVIF:   fmt_name = "AVIF"; break;
                default: break;
            }

            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted("Format");
            ImGui::TableNextColumn(); ImGui::Text("%s", fmt_name);

            if (!info.color_space.empty()) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted("Color space");
                ImGui::TableNextColumn(); ImGui::Text("%s", info.color_space.c_str());
            }

            ImGui::EndTable();
        }

        // ICC profile info
        if (!info.icc_profile_name.empty()) {
            ImGui::Spacing();
            ImGui::Text("ICC: %s", info.icc_profile_name.c_str());
        } else {
            ImGui::TextDisabled("No ICC profile");
        }
    }
}

void PropertiesPanel::render_measurements(
    Viewport& viewport,
    const std::vector<const char*>& slot_labels,
    std::vector<int>& out_deleted_ids,
    bool& out_clear_all) {
    out_deleted_ids.clear();
    out_clear_all = false;

    const auto& items = viewport.measurements();

    if (items.empty()) {
        ImGui::TextDisabled("No measurements.");
        ImGui::TextDisabled("Toggle \"Measure\" in the viewport toolbar, then");
        ImGui::TextDisabled("left-drag a region to record its source-image size.");
        return;
    }

    int hover_id = viewport.hover_measurement_id();

    if (ImGui::BeginTable("##measurements_table", 4,
                           ImGuiTableFlags_BordersInnerV |
                           ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("#",      ImGuiTableColumnFlags_WidthFixed, 36.0f);
        ImGui::TableSetupColumn("Size",   ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("",       ImGuiTableColumnFlags_WidthFixed, 28.0f);
        ImGui::TableHeadersRow();

        int pending_delete = -1;
        for (const auto& m : items) {
            ImGui::TableNextRow();
            bool is_hover = (hover_id == m.id);
            if (is_hover) {
                // Subtle cyan wash to mirror the viewport's accent color
                // on the hovered rect.
                ImU32 tint = IM_COL32(0x33, 0xE6, 0xFF, 32);
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, tint);
            }

            ImGui::PushID(m.id);

            ImGui::TableNextColumn();
            ImGui::Text("%d", m.id);

            ImGui::TableNextColumn();
            int w_px = static_cast<int>(std::round(m.x1 - m.x0));
            int h_px = static_cast<int>(std::round(m.y1 - m.y0));
            ImGui::Text("%d x %d px", w_px, h_px);

            ImGui::TableNextColumn();
            const char* src = "(unknown)";
            if (m.source_cell_index >= 0 &&
                m.source_cell_index < static_cast<int>(slot_labels.size()) &&
                slot_labels[m.source_cell_index]) {
                src = slot_labels[m.source_cell_index];
            }
            ImGui::TextUnformatted(src);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Native %d x %d px\nRect: (%.0f, %.0f) to (%.0f, %.0f)",
                                  m.src_tex_w, m.src_tex_h,
                                  m.x0, m.y0, m.x1, m.y1);
            }

            ImGui::TableNextColumn();
            if (ImGui::SmallButton("x")) {
                pending_delete = m.id;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Remove measurement #%d", m.id);
            }

            ImGui::PopID();
        }
        ImGui::EndTable();

        if (pending_delete >= 0) {
            out_deleted_ids.push_back(pending_delete);
            viewport.remove_measurement(pending_delete);
        }
    }

    ImGui::Spacing();
    float button_w = 90.0f;
    float avail = ImGui::GetContentRegionAvail().x;
    if (avail > button_w) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - button_w);
    }
    if (ImGui::Button("Clear All", ImVec2(button_w, 0))) {
        out_clear_all = true;
        viewport.clear_measurements();
    }
}

} // namespace idiff
