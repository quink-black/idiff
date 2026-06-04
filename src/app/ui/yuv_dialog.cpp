#include "app/ui/yuv_dialog.h"

#include <imgui.h>

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace idiff {

namespace {

// A predefined Combo entry: a human-readable label paired with the
// FFmpeg name actually written into YuvStreamParams.
struct ComboOption {
    const char* label;
    const char* value;
};

// Pixel format presets.  The dialog appends a synthetic "Custom..."
// entry so users can still type any FFmpeg pixel format name FFmpeg
// supports, even ones not enumerated here.
constexpr ComboOption kPixelFormats[] = {
    {"YUV420P 8-bit (yuv420p)",       "yuv420p"},
    {"YUV422P 8-bit (yuv422p)",       "yuv422p"},
    {"YUV444P 8-bit (yuv444p)",       "yuv444p"},
    {"YUV420P 10-bit (yuv420p10le)",  "yuv420p10le"},
    {"YUV422P 10-bit (yuv422p10le)",  "yuv422p10le"},
    {"YUV444P 10-bit (yuv444p10le)",  "yuv444p10le"},
    {"NV12 (nv12)",                   "nv12"},
    {"NV21 (nv21)",                   "nv21"},
    {"P010 10-bit (p010le)",          "p010le"},
};

constexpr ComboOption kColorRanges[] = {
    {"Limited / TV (tv)", "tv"},
    {"Full / PC (pc)",    "pc"},
};

constexpr ComboOption kColorMatrices[] = {
    {"BT.601 (smpte170m)",   "smpte170m"},
    {"BT.709 (bt709)",       "bt709"},
    {"BT.2020 NCL (bt2020nc)", "bt2020nc"},
};

constexpr ComboOption kColorPrimaries[] = {
    {"BT.601 (smpte170m)", "smpte170m"},
    {"BT.709 (bt709)",     "bt709"},
    {"BT.2020 (bt2020)",   "bt2020"},
};

constexpr ComboOption kTransfers[] = {
    {"BT.709 / SDR (bt709)",   "bt709"},
    {"sRGB (iec61966-2-1)",    "iec61966-2-1"},
    {"PQ / ST 2084 (smpte2084)", "smpte2084"},
    {"HLG (arib-std-b67)",     "arib-std-b67"},
};

// Find the option whose value equals `current`.  Returns -1 when not
// found, signaling the caller to fall back (Custom for pixel format,
// or default-to-first for the closed enums).
template <std::size_t N>
int find_option_index(const ComboOption (&options)[N],
                      const std::string& current) {
    for (std::size_t i = 0; i < N; ++i) {
        if (current == options[i].value) return static_cast<int>(i);
    }
    return -1;
}

// Render a closed-set Combo whose values are constrained to entries in
// `options`.  When `value` matches no preset the first entry is
// selected and written back so the params never carry a stale name.
template <std::size_t N>
void render_closed_combo(const char* label,
                         const ComboOption (&options)[N],
                         std::string& value) {
    int idx = find_option_index(options, value);
    if (idx < 0) {
        idx = 0;
        value = options[0].value;
    }
    if (ImGui::BeginCombo(label, options[idx].label)) {
        for (int i = 0; i < static_cast<int>(N); ++i) {
            const bool selected = (i == idx);
            if (ImGui::Selectable(options[i].label, selected)) {
                value = options[i].value;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
}

// Pixel format Combo + optional Custom text input.  The synthetic
// "Custom..." entry is appended to the preset list; selecting it
// reveals an InputText for advanced users who need an FFmpeg name
// not covered by the presets (e.g. yuv420p12le).  The buffer is
// rewritten to params every frame so typed input is never silently
// dropped.
void render_pixel_format_widget(std::string& value) {
    int idx = find_option_index(kPixelFormats, value);
    const bool is_custom = (idx < 0);
    constexpr int kCustomIdx = static_cast<int>(
        sizeof(kPixelFormats) / sizeof(kPixelFormats[0]));

    const char* preview = is_custom
        ? "Custom..."
        : kPixelFormats[idx].label;

    if (ImGui::BeginCombo("Pixel format", preview)) {
        for (int i = 0; i < kCustomIdx; ++i) {
            const bool selected = (i == idx);
            if (ImGui::Selectable(kPixelFormats[i].label, selected)) {
                value = kPixelFormats[i].value;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::Separator();
        if (ImGui::Selectable("Custom...", is_custom)) {
            // Switching into custom mode keeps the existing string so
            // the user can edit it; if it matched a preset, leave it
            // as-is and let them rename.
        }
        if (is_custom) ImGui::SetItemDefaultFocus();
        ImGui::EndCombo();
    }

    if (is_custom) {
        char buf[64];
        std::strncpy(buf, value.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        if (ImGui::InputText("FFmpeg pixel format name",
                             buf, sizeof(buf))) {
            value = buf;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Any FFmpeg pixel format name, e.g. yuv420p12le, "
                "yuva420p, gbrp10le.");
        }
    }
}

}  // namespace

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

        // Plain numeric entry; step=0 hides the +/- buttons that are
        // meaningless for image dimensions.
        ImGui::InputInt("Width",  &params.width,  0, 0);
        ImGui::InputInt("Height", &params.height, 0, 0);

        render_pixel_format_widget(params.pixel_format);
        render_closed_combo("Color range",     kColorRanges,    params.color_range);
        render_closed_combo("Color matrix",    kColorMatrices,  params.color_matrix);
        render_closed_combo("Color primaries", kColorPrimaries, params.color_primaries);
        render_closed_combo("Transfer",        kTransfers,      params.transfer);

        // Show file size when dimensions look usable.
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
