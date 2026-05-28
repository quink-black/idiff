#include "app/ui/image_list.h"

#include "app/app.h"             // ImageEntry
#include "app/sr_infer_engine.h" // SREngineStatus
#include "domain/comparison_config_service.h"
#include "domain/diff_service.h"
#include "domain/selection_model.h"
#include "domain/sr_task_service.h"

#include <imgui.h>

#include <cstdio>
#include <string>

namespace idiff {

void render_image_list(const ImageListInputs& in) {
    auto& entries = *in.entries;
    auto& selection = *in.selection;
    auto& diff_service = *in.diff_service;
    const auto& sr_service = *in.sr_service;

    ImGui::SetNextWindowSize(ImVec2(220, 400), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Images", in.show_image_list)) {
        ImGui::End();
        return;
    }

    if (ImGui::Button("+ Add Images", ImVec2(-1, 0))) {
        if (in.on_open_files) in.on_open_files();
    }

    // Group selector, only shown when a comparison-config is active.
    // Switching the combo triggers an on-demand download + load of the
    // selected group; only that one group's pixels are kept resident.
    if (in.comparison_config &&
        in.comparison_config->current_index() >= 0 &&
        in.comparison_config->has_config()) {
        const auto& groups = in.comparison_config->config().groups;
        int idx = in.comparison_config->current_index();
        std::string preview = (idx >= 0 &&
                               idx < static_cast<int>(groups.size()))
            ? groups[idx].name : std::string("(none)");
        preview += " (" +
            std::to_string((idx >= 0 && idx < static_cast<int>(groups.size()))
                           ? groups[idx].items.size() : 0) + ")";
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##group", preview.c_str())) {
            for (int i = 0; i < static_cast<int>(groups.size()); ++i) {
                std::string label = groups[i].name + "  (" +
                    std::to_string(groups[i].items.size()) + ")";
                bool selected = (i == idx);
                if (ImGui::Selectable(label.c_str(), selected)) {
                    if (in.on_switch_comparison_group) {
                        in.on_switch_comparison_group(i);
                    }
                }
                if (!groups[i].description.empty() &&
                    ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", groups[i].description.c_str());
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::TextDisabled("Group %d / %d",
                            idx + 1, static_cast<int>(groups.size()));
    }

    if (selection.size() >= 2) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear")) {
            selection.clear();
            diff_service.mark_dirty();
        }
    }

    ImGui::Separator();

    if (!selection.empty()) {
        ImGui::TextColored(ImVec4(0.40f, 0.80f, 1.00f, 1.00f),
                           "%zu selected", selection.size());
    }

    float list_height = ImGui::GetContentRegionAvail().y;
    if (ImGui::BeginChild("##image_list_child", ImVec2(0, list_height), false)) {
        // The smallest selected index is the reference image used by
        // overlay / diff; every other selected entry is a partner.
        int ref_idx = -1;
        if (in.get_ref_index) in.get_ref_index(ref_idx);

        for (int i = 0; i < static_cast<int>(entries.size()); i++) {
            auto& entry = entries[i];
            ImGui::PushID(i);

            bool is_sel = selection.contains(i);
            const char* ref_tag = (i == ref_idx) ? "Ref" : nullptr;

            if (is_sel) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.40f, 0.80f, 1.00f, 1.00f));
            }

            bool checked = is_sel;
            if (ImGui::Checkbox("##sel", &checked)) {
                if (checked) {
                    selection.insert(i);
                } else {
                    selection.erase(i);
                }
                // Selection change affects upscale targets for all selected images
                for (int s : selection.indices()) {
                    if (s >= 0 && s < static_cast<int>(entries.size())) {
                        entries[s].texture_dirty = true;
                    }
                }
                diff_service.mark_dirty();
                // Statistics panel cache is pointer-keyed and self-prunes,
                // so no explicit invalidation is needed on selection change.
            }

            ImGui::SameLine();

            // Draw the Ref tag as a pill in front of the label so the
            // user can tell which selected entry feeds overlay / diff
            // as the reference image.
            if (ref_tag) {
                ImVec4 pill_col = ImVec4(0.30f, 0.70f, 1.00f, 1.00f);
                ImGui::PushStyleColor(ImGuiCol_Button, pill_col);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, pill_col);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, pill_col);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
                ImGui::SmallButton(ref_tag);
                ImGui::PopStyleColor(4);
                ImGui::SameLine();
            }

            // Selectable for the label -- also serves as drag source/target
            ImGui::Selectable(entry.display_label.c_str(), is_sel,
                              ImGuiSelectableFlags_AllowOverlap);

            // Show SR progress indicator if this entry is being processed
            for (const auto& task : sr_service.tasks()) {
                if (task.input_path == entry.path && task.engine &&
                    task.engine->get_status() == SREngineStatus::Running) {
                    ImGui::SameLine();
                    float p = task.engine->get_progress();
                    if (p >= 0) {
                        ImGui::TextColored(
                            ImVec4(0.4f, 0.8f, 1.0f, 1.0f),
                            "SR %d%%", static_cast<int>(p * 100));
                    } else {
                        ImGui::TextColored(
                            ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "SR...");
                    }
                    break;
                }
            }

            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", entry.path.c_str());
            }

            // Drag source
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                ImGui::SetDragDropPayload("IMAGE_REORDER", &i, sizeof(int));
                ImGui::Text("%s", entry.display_label.c_str());
                ImGui::EndDragDropSource();
            }

            // Drop target
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("IMAGE_REORDER")) {
                    int src = *static_cast<const int*>(payload->Data);
                    if (in.on_move_entry) in.on_move_entry(src, i);
                }
                ImGui::EndDragDropTarget();
            }

            if (ImGui::BeginPopupContextItem("entry_ctx")) {
                // Only YUV streams can have their decoder parameters
                // re-edited after load; other sources (still images)
                // don't have user-visible parameters.
                if (in.entry_is_yuv && in.entry_is_yuv(i)) {
                    if (ImGui::MenuItem("Edit YUV parameters...")) {
                        if (in.on_edit_yuv_entry) in.on_edit_yuv_entry(i);
                    }
                    ImGui::Separator();
                }
                // Super Resolution -- only shown when an upscaler is
                // detected and at least one entry is selected.
                if (in.sr_enabled && !selection.empty()) {
                    bool any_running = false;
                    for (const auto& task : sr_service.tasks()) {
                        if (task.engine &&
                            task.engine->get_status() == SREngineStatus::Running) {
                            any_running = true;
                            break;
                        }
                    }
                    if (any_running) {
                        // Collect progress info for the tooltip.
                        float total_progress = 0.0f;
                        int running_count = 0;
                        for (const auto& task : sr_service.tasks()) {
                            if (task.engine &&
                                task.engine->get_status() == SREngineStatus::Running) {
                                float p = task.engine->get_progress();
                                if (p >= 0) total_progress += p;
                                ++running_count;
                            }
                        }
                        char sr_label[64];
                        if (running_count > 0 && total_progress >= 0) {
                            int pct = static_cast<int>(
                                total_progress / running_count * 100.0f);
                            std::snprintf(sr_label, sizeof(sr_label),
                                          "Super Resolution... (%d%%)", pct);
                        } else {
                            std::snprintf(sr_label, sizeof(sr_label),
                                          "Super Resolution... (running)");
                        }
                        ImGui::MenuItem(sr_label, nullptr, false, false);
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                            ImGui::BeginTooltip();
                            if (running_count == 1) {
                                ImGui::TextUnformatted(
                                    "A super resolution task is running.");
                            } else {
                                char buf[64];
                                std::snprintf(buf, sizeof(buf),
                                    "%d super resolution tasks are running.",
                                    running_count);
                                ImGui::TextUnformatted(buf);
                            }
                            ImGui::TextDisabled(
                                "Please wait for it to finish before\n"
                                "starting a new task.");
                            ImGui::EndTooltip();
                        }
                    } else {
                        if (ImGui::MenuItem("Super Resolution...")) {
                            // Use only the right-clicked entry as the SR
                            // input.  Previously we gathered all selected
                            // entries, which caused stale selections (e.g.
                            // auto-selected input+output from a previous
                            // SR run) to spawn duplicate tasks.
                            if (in.on_open_sr_dialog) in.on_open_sr_dialog(i);
                        }
                    }
                    ImGui::Separator();
                }
                // Selection operations
                if (!entries.empty()) {
                    if (ImGui::MenuItem("Select All")) {
                        if (in.on_select_all) in.on_select_all();
                    }
                }
                if (ImGui::MenuItem("Select Only This")) {
                    if (in.on_select_only_this) in.on_select_only_this(i);
                }
                bool already_ref = (i == ref_idx);
                if (ImGui::MenuItem("Mark as Reference", nullptr, false,
                                    !already_ref)) {
                    if (in.on_mark_as_reference) in.on_mark_as_reference(i);
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip(
                        "Move this image to the top of the list and use\n"
                        "it as the reference for overlay / diff.");
                }
                if (!entries.empty() && selection.size() < entries.size()) {
                    if (ImGui::MenuItem("Invert Selection")) {
                        if (in.on_invert_selection) in.on_invert_selection();
                    }
                }
                if (!selection.empty()) {
                    if (ImGui::MenuItem("Unselect All")) {
                        if (in.on_unselect_all) in.on_unselect_all();
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Reload", "F5")) {
                    if (in.on_reload_entry) in.on_reload_entry(i);
                }
                if (entries.size() > 1) {
                    if (ImGui::MenuItem("Reload All")) {
                        if (in.on_reload_all) in.on_reload_all();
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Remove")) {
                    if (in.on_remove_entry) in.on_remove_entry(i);
                }
                if (selection.size() >= 2) {
                    char sel_label[64];
                    std::snprintf(sel_label, sizeof(sel_label),
                                  "Remove Selected (%zu)", selection.size());
                    if (ImGui::MenuItem(sel_label)) {
                        if (in.on_remove_selected) in.on_remove_selected();
                    }
                }
                if (!entries.empty()) {
                    if (ImGui::MenuItem("Remove All")) {
                        if (in.on_remove_all) in.on_remove_all();
                    }
                }
                ImGui::EndPopup();
            }

            if (is_sel) {
                ImGui::PopStyleColor();
            }

            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    if (entries.empty()) {
        ImGui::TextDisabled("No images loaded.");
        ImGui::TextDisabled("Ctrl+O or click '+' to add.");
    }

    ImGui::End();
}

} // namespace idiff
