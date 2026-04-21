#include "app/sr_dialog.h"
#include "app/settings.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include <imgui.h>

namespace idiff {

std::filesystem::path sr_default_output_path(
    const std::filesystem::path& input, int scale) {
    auto stem = input.stem().string();
    auto ext = input.extension().string();
    if (ext.empty()) ext = ".png";
    auto name = stem + "_sr_" + std::to_string(scale) + "x" + ext;
    return input.parent_path() / name;
}

void sr_dialog_open(SRDialogState& state,
                    const std::vector<std::filesystem::path>& inputs,
                    const AppSettings& settings) {
    state.open = true;
    state.confirmed = false;
    state.task_params.clear();
    state.input_paths = inputs;
    state.input_names.clear();
    state.output_path_overrides.clear();

    for (const auto& p : inputs) {
        state.input_names.push_back(p.filename().string());
        state.output_path_overrides.emplace_back();
    }

    // Pre-fill from persistent settings
    state.scale = settings.sr_scale;
    state.tile_size = settings.sr_tile_size;
    state.tile_overlap = settings.sr_tile_overlap;
    std::strncpy(state.model_buf, settings.sr_model.c_str(),
                 sizeof(state.model_buf) - 1);
    state.model_buf[sizeof(state.model_buf) - 1] = '\0';
    std::strncpy(state.color_correction_buf, settings.sr_color_correction.c_str(),
                 sizeof(state.color_correction_buf) - 1);
    state.color_correction_buf[sizeof(state.color_correction_buf) - 1] = '\0';
}

bool sr_dialog_render(SRDialogState& state) {
    if (!state.open) return false;

    ImGui::OpenPopup("Super Resolution###sr_dialog");

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(500, 0), ImGuiCond_Appearing);

    bool confirmed = false;

    if (ImGui::BeginPopupModal("Super Resolution###sr_dialog",
                               &state.open,
                               ImGuiWindowFlags_NoResize)) {
        // Input info
        ImGui::TextDisabled("Input images: %zu", state.input_paths.size());
        if (state.input_paths.size() <= 5) {
            for (const auto& name : state.input_names) {
                ImGui::BulletText("%s", name.c_str());
            }
        } else {
            for (int i = 0; i < 3; ++i) {
                ImGui::BulletText("%s", state.input_names[i].c_str());
            }
            ImGui::BulletText("... and %zu more",
                              state.input_paths.size() - 3);
        }

        ImGui::Separator();

        // Scale selection
        const char* scale_items[] = {"2x", "4x"};
        int scale_values[] = {2, 4};
        int current_scale_idx = (state.scale == 4) ? 1 : 0;
        ImGui::Text("Scale");
        if (ImGui::Combo("##scale", &current_scale_idx,
                          scale_items, IM_ARRAYSIZE(scale_items))) {
            state.scale = scale_values[current_scale_idx];
        }

        // Tile size
        ImGui::Text("Tile Size");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputInt("##tile_size", &state.tile_size);
        if (state.tile_size < 0) state.tile_size = 0;

        // Tile overlap
        ImGui::Text("Tile Overlap");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputInt("##tile_overlap", &state.tile_overlap);
        if (state.tile_overlap < 0) state.tile_overlap = 0;

        // Model
        ImGui::Text("Model");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##model", state.model_buf, sizeof(state.model_buf));

        // Color correction
        ImGui::Text("Color Correction");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##color_correction",
                          state.color_correction_buf,
                          sizeof(state.color_correction_buf));

        ImGui::Separator();

        // Output path preview
        ImGui::TextDisabled("Output paths:");
        int show_count = std::min<int>(
            static_cast<int>(state.input_paths.size()), 3);
        for (int i = 0; i < show_count; ++i) {
            auto out = sr_default_output_path(
                state.input_paths[i], state.scale);
            ImGui::BulletText("%s", out.filename().string().c_str());
        }
        if (static_cast<int>(state.input_paths.size()) > show_count) {
            ImGui::BulletText("... and %zu more",
                              state.input_paths.size() - show_count);
        }

        // Warning if output file exists
        for (const auto& input : state.input_paths) {
            auto out = sr_default_output_path(input, state.scale);
            if (std::filesystem::exists(out)) {
                ImGui::TextColored(
                    ImVec4(1.0f, 0.8f, 0.0f, 1.0f),
                    "Warning: %s already exists and will be overwritten",
                    out.filename().string().c_str());
                break;  // One warning is enough
            }
        }

        ImGui::Separator();

        // Buttons
        float button_width = 120.0f;
        float spacing = ImGui::GetStyle().ItemSpacing.x;
        float total_width = button_width * 2 + spacing;
        ImGui::SetCursorPosX(
            (ImGui::GetWindowWidth() - total_width) * 0.5f);

        if (ImGui::Button("Start", ImVec2(button_width, 0))) {
            // Build task params for each input image
            state.task_params.clear();
            for (const auto& input : state.input_paths) {
                SRTaskParams params;
                params.input_path = input;
                params.output_path = sr_default_output_path(input, state.scale);
                params.scale = state.scale;
                params.tile_size = state.tile_size;
                params.tile_overlap = state.tile_overlap;
                params.model = state.model_buf;
                params.color_correction = state.color_correction_buf;
                state.task_params.push_back(std::move(params));
            }
            state.confirmed = true;
            confirmed = true;
            state.open = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(button_width, 0))) {
            state.open = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    return confirmed;
}

} // namespace idiff