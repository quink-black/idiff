#include "app/ui/dialogs.h"

#include <imgui.h>

#include <cstdio>
#include <filesystem>

namespace idiff {

void render_error_dialog(ErrorDialogState& state) {
    if (!state.visible) return;

    if (state.needs_open) {
        ImGui::OpenPopup("Error###error_dialog");
        state.needs_open = false;
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(480, 0), ImGuiCond_Appearing);

    if (ImGui::BeginPopupModal("Error###error_dialog",
                               &state.visible,
                               ImGuiWindowFlags_NoResize)) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s",
                           state.title.c_str());
        ImGui::Separator();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 440);
        ImGui::TextUnformatted(state.message.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        ImGui::Separator();
        float button_width = 120.0f;
        ImGui::SetCursorPosX(
            (ImGui::GetWindowWidth() - button_width) * 0.5f);
        if (ImGui::Button("OK", ImVec2(button_width, 0))) {
            state.visible = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void render_reload_dialog(ReloadDialogState& state) {
    if (!state.visible) return;

    if (state.needs_open) {
        ImGui::OpenPopup("File Changed###reload_dialog");
        state.needs_open = false;
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(440, 0), ImGuiCond_Appearing);

    if (ImGui::BeginPopupModal("File Changed###reload_dialog",
                               &state.visible,
                               ImGuiWindowFlags_NoResize)) {
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f),
                           "File(s) changed on disk");
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 400);

        int count = static_cast<int>(state.changed_paths.size());
        if (count == 1) {
            std::string name =
                std::filesystem::path(state.changed_paths[0])
                    .filename().string();
            ImGui::Text("%s has been modified externally.", name.c_str());
        } else {
            ImGui::Text("%d files have been modified externally:", count);
            ImGui::Spacing();
            // Show up to 5 filenames
            int show = (count > 5) ? 5 : count;
            for (int i = 0; i < show; ++i) {
                std::string name =
                    std::filesystem::path(state.changed_paths[i])
                        .filename().string();
                ImGui::BulletText("%s", name.c_str());
            }
            if (count > 5) {
                ImGui::BulletText("... and %d more", count - 5);
            }
        }

        ImGui::Spacing();
        ImGui::TextUnformatted("Reload from disk?");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        ImGui::Separator();

        float button_width = 120.0f;
        float spacing = ImGui::GetStyle().ItemSpacing.x;
        float total_width = button_width * 2 + spacing;
        ImGui::SetCursorPosX(
            (ImGui::GetWindowWidth() - total_width) * 0.5f);

        if (ImGui::Button("Reload", ImVec2(button_width, 0))) {
            state.reload_requested = true;
            state.visible = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Ignore", ImVec2(button_width, 0))) {
            state.changed_paths.clear();
            state.visible = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

} // namespace idiff
