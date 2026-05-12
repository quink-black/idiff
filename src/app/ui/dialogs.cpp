#include "app/ui/dialogs.h"

#include "app/sr_infer_engine.h"
#include "domain/sr_task_service.h"

#include <imgui.h>

#include <cstdio>

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

void render_quit_confirm_dialog(QuitConfirmDialogState& state,
                                SrTaskService& sr_service) {
    if (!state.visible) return;

    if (state.needs_open) {
        ImGui::OpenPopup("Quit###quit_confirm_dialog");
        state.needs_open = false;
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(400, 0), ImGuiCond_Appearing);

    if (ImGui::BeginPopupModal("Quit###quit_confirm_dialog",
                               &state.visible,
                               ImGuiWindowFlags_NoResize)) {
        int running_count = 0;
        for (const auto& task : sr_service.tasks()) {
            if (task.engine &&
                task.engine->get_status() == SREngineStatus::Running) {
                ++running_count;
            }
        }

        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
                           "Super resolution in progress");
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 360);
        if (running_count == 1) {
            ImGui::TextUnformatted(
                "A super resolution task is still running. "
                "Quitting now will cancel it and the output file "
                "may be incomplete.");
        } else {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "%d super resolution tasks are still running. "
                          "Quitting now will cancel them and output files "
                          "may be incomplete.", running_count);
            ImGui::TextUnformatted(buf);
        }
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        ImGui::Separator();

        float button_width = 140.0f;
        float spacing = ImGui::GetStyle().ItemSpacing.x;
        float total_width = button_width * 2 + spacing;
        ImGui::SetCursorPosX(
            (ImGui::GetWindowWidth() - total_width) * 0.5f);

        if (ImGui::Button("Quit Anyway", ImVec2(button_width, 0))) {
            sr_service.cancel_all();
            state.confirmed = true;
            state.visible = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Keep Waiting", ImVec2(button_width, 0))) {
            state.visible = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

} // namespace idiff
