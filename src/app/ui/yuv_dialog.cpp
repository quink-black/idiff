#include "app/ui/yuv_dialog.h"

#include <imgui.h>

#include <cstddef>
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
            // Entry disappeared (removed, reordered out of range).
            // Abort silently so the dialog returns to idle.
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
        // Edit mode: caller already seeded params; do not overwrite.
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

        const char* fmt_items[] = {
            "YUV420P (I420)", "YUV422P", "YUV444P",
            "YUV420P10", "YUV422P10", "YUV444P10",
            "P010", "NV16"
        };
        int fmt_idx = static_cast<int>(params.pixel_format);
        if (ImGui::Combo("Pixel format", &fmt_idx, fmt_items,
                         IM_ARRAYSIZE(fmt_items))) {
            params.pixel_format = static_cast<YuvPixelFormat>(fmt_idx);
        }

        const char* range_items[] = { "Limited (TV, 16-235)", "Full (PC, 0-255)" };
        int range_idx = static_cast<int>(params.color_range);
        if (ImGui::Combo("Color range", &range_idx, range_items,
                         IM_ARRAYSIZE(range_items))) {
            params.color_range = static_cast<YuvColorRange>(range_idx);
        }

        const char* matrix_items[] = { "BT.601", "BT.709", "BT.2020 NCL" };
        int matrix_idx = static_cast<int>(params.color_matrix);
        if (ImGui::Combo("Color matrix", &matrix_idx, matrix_items,
                         IM_ARRAYSIZE(matrix_items))) {
            params.color_matrix = static_cast<YuvColorMatrix>(matrix_idx);
        }

        const char* primaries_items[] = { "BT.601", "BT.709", "BT.2020" };
        int primaries_idx = static_cast<int>(params.color_primaries);
        if (ImGui::Combo("Color primaries", &primaries_idx, primaries_items,
                         IM_ARRAYSIZE(primaries_items))) {
            params.color_primaries = static_cast<YuvColorPrimaries>(primaries_idx);
        }

        std::size_t frame_bytes = yuv_frame_size_bytes(params);
        if (frame_bytes == 0) {
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1),
                "Invalid: width/height must be positive (and even for subsampled formats)");
        } else {
            std::error_code ec;
            auto fsize = std::filesystem::file_size(current_path, ec);
            if (ec) {
                ImGui::TextDisabled("Frame size: %zu bytes",
                                     static_cast<size_t>(frame_bytes));
            } else {
                int fc = static_cast<int>(fsize / frame_bytes);
            ImGui::Text("Frame size: %zu bytes (%d-bit)  |  File has %d frame(s)",
                        static_cast<size_t>(frame_bytes),
                        yuv_pixel_format_bit_depth(params.pixel_format),
                        fc);
                if (fsize % frame_bytes != 0) {
                    ImGui::TextColored(ImVec4(1, 0.7f, 0.2f, 1),
                        "Warning: file size is not an exact multiple of frame size");
                }
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
