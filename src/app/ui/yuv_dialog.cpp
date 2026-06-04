#include "app/ui/yuv_dialog.h"

#include <imgui.h>

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace idiff {

void render_yuv_params_dialog(YuvDialogState& state,
                              const YuvDialogCallbacks& callbacks) {
    const bool editing = state.editing_entry_idx >= 0;
    if (!editing && state.pending_paths.empty()) return;

    std::string current_path;
    if (editing) {
        current_path = callbacks.resolve_entry_path
            ? callbacks.resolve_entry_path(state.editing_entry_idx)
            : std::string();
        if (current_path.empty()) {
            state.editing_entry_idx = -1;
            return;
        }
    } else {
        current_path = state.pending_paths.front();
    }

    if (state.needs_open) {
        if (!editing) {
            state.params = callbacks.default_load_params
                ? callbacks.default_load_params()
                : YuvStreamParams{};
            guess_yuv_params_from_filename(current_path, state.params);
        }
        ImGui::OpenPopup("YUV Parameters");
        state.needs_open = false;
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("YUV Parameters", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(editing
            ? "Edit decoder parameters for:"
            : "Configure decoder parameters for:");
        ImGui::TextDisabled("%s", current_path.c_str());
        ImGui::Separator();

        auto& params = state.params;

        ImGui::InputInt("Width",  &params.width);
        ImGui::InputInt("Height", &params.height);

        // Pixel format: FFmpeg name text input.
        char pix_fmt_buf[64];
        std::strncpy(pix_fmt_buf, params.pixel_format.c_str(),
                     sizeof(pix_fmt_buf) - 1);
        pix_fmt_buf[sizeof(pix_fmt_buf) - 1] = '\0';
        if (ImGui::InputText("Pixel format", pix_fmt_buf, sizeof(pix_fmt_buf),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            params.pixel_format = pix_fmt_buf;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("FFmpeg pixel format name, e.g. yuv420p, yuv420p10le, nv12, p010le, yuv444p12le");
        }

        // Color range: FFmpeg name text input.
        char range_buf[16];
        std::strncpy(range_buf, params.color_range.c_str(),
                     sizeof(range_buf) - 1);
        range_buf[sizeof(range_buf) - 1] = '\0';
        if (ImGui::InputText("Color range", range_buf, sizeof(range_buf),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            params.color_range = range_buf;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("FFmpeg color range: tv (limited) or pc (full)");
        }

        // Color matrix: FFmpeg name text input.
        char matrix_buf[64];
        std::strncpy(matrix_buf, params.color_matrix.c_str(),
                     sizeof(matrix_buf) - 1);
        matrix_buf[sizeof(matrix_buf) - 1] = '\0';
        if (ImGui::InputText("Color matrix", matrix_buf, sizeof(matrix_buf),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            params.color_matrix = matrix_buf;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("FFmpeg color space name, e.g. bt709, smpte170m, bt2020-nccl");
        }

        // Color primaries: FFmpeg name text input.
        char primaries_buf[64];
        std::strncpy(primaries_buf, params.color_primaries.c_str(),
                     sizeof(primaries_buf) - 1);
        primaries_buf[sizeof(primaries_buf) - 1] = '\0';
        if (ImGui::InputText("Color primaries", primaries_buf,
                             sizeof(primaries_buf),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            params.color_primaries = primaries_buf;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("FFmpeg color primaries name, e.g. bt709, smpte170m, bt2020");
        }

        // Transfer: FFmpeg name text input.
        char transfer_buf[64];
        std::strncpy(transfer_buf, params.transfer.c_str(),
                     sizeof(transfer_buf) - 1);
        transfer_buf[sizeof(transfer_buf) - 1] = '\0';
        if (ImGui::InputText("Transfer", transfer_buf, sizeof(transfer_buf),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            params.transfer = transfer_buf;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("FFmpeg transfer name, e.g. bt709, smpte2084, arib-std-b67");
        }

        // Show file info if available.
        if (params.width > 0 && params.height > 0) {
            std::error_code ec;
            auto fsize = std::filesystem::file_size(current_path, ec);
            if (!ec) {
                ImGui::TextDisabled("File: %zu bytes",
                                    static_cast<size_t>(fsize));
            }
        }

        ImGui::Separator();

        bool confirmed = false;
        bool skipped   = false;
        bool cancelled = false;
        const char* confirm_label = editing ? "Apply" : "Load";
        if (ImGui::Button(confirm_label, ImVec2(100, 0))) {
            confirmed = true;
        }
        ImGui::SameLine();
        if (editing) {
            if (ImGui::Button("Cancel", ImVec2(100, 0))) {
                cancelled = true;
            }
        } else {
            if (ImGui::Button("Skip", ImVec2(100, 0))) {
                skipped = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel all", ImVec2(100, 0))) {
                state.pending_paths.clear();
                ImGui::CloseCurrentPopup();
            }
        }

        if (confirmed) {
            if (editing) {
                int idx = state.editing_entry_idx;
                state.editing_entry_idx = -1;
                if (callbacks.on_edit_apply) {
                    callbacks.on_edit_apply(idx, params);
                }
            } else {
                if (callbacks.on_load_confirm) {
                    callbacks.on_load_confirm(current_path, params);
                }
                state.pending_paths.erase(state.pending_paths.begin());
                if (!state.pending_paths.empty()) {
                    state.needs_open = true;
                }
            }
            ImGui::CloseCurrentPopup();
        } else if (skipped) {
            state.pending_paths.erase(state.pending_paths.begin());
            ImGui::CloseCurrentPopup();
            if (!state.pending_paths.empty()) {
                state.needs_open = true;
            }
        } else if (cancelled) {
            state.editing_entry_idx = -1;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

} // namespace idiff
