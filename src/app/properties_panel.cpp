#include "app/properties_panel.h"

#include <imgui.h>

#include <cstdio>

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

    // Header shows "A — filename" / "B — filename" so the user can tell which
    // selected image is used as A / B in overlay and diff modes.
    char header[512];
    if (name && name[0]) {
        std::snprintf(header, sizeof(header), "%s \xe2\x80\x94 %s", slot_label, name);
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
            ImGui::TableNextColumn(); ImGui::Text("%d", info.bit_depth);

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

} // namespace idiff
