#include "app/ui/image_list.h"

#include "app/app.h"             // ImageEntry
#include "app/sr_infer_engine.h" // SREngineStatus
#include "domain/comparison_config_service.h"
#include "domain/diff_service.h"
#include "domain/group_key.h"
#include "domain/selection_model.h"
#include "domain/sr_task_service.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace idiff {

void render_image_list(const ImageListInputs& in) {
    auto& entries = *in.entries;
    auto& selection = *in.selection;
    auto& diff_service = *in.diff_service;
    const auto& sr_service = *in.sr_service;

    // Track whether the panel close button was clicked so we can
    // persist the change.
    bool was_visible = in.show_image_list ? *in.show_image_list : true;

    ImGui::SetNextWindowSize(ImVec2(220, 400), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Images", in.show_image_list)) {
        ImGui::End();
        return;
    }

    if (ImGui::Button("+ Add Images", ImVec2(-1, 0))) {
        if (in.on_open_files) in.on_open_files();
    }

    // Group by Name toggle -- directly in the panel for easy access.
    if (in.group_by_name_ptr) {
        if (ImGui::Checkbox("Group by Name", in.group_by_name_ptr)) {
            if (in.on_settings_changed) in.on_settings_changed();
        }
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

        const bool group_mode = in.group_by_name_ptr &&
                                *in.group_by_name_ptr;

        // Pre-compute group keys so group boundaries can be drawn
        // without recomputing keys on every iteration.
        std::vector<std::string> group_keys;
        if (group_mode) {
            group_keys.reserve(entries.size());
            for (const auto& e : entries)
                group_keys.push_back(group_key_from_filename(e.filename));
        }

        int group_color_idx = 0;  // alternates at each group boundary

        for (int i = 0; i < static_cast<int>(entries.size()); i++) {
            auto& entry = entries[i];
            ImGui::PushID(i);

            // Group-mode visual indicators: separator between groups
            // and alternating row tint.
            if (group_mode) {
                if (i > 0 && group_keys[i] != group_keys[i - 1]) {
                    ImGui::Separator();
                    ++group_color_idx;
                }
                // Subtle background tint for alternating groups.
                if (group_color_idx % 2 == 1) {
                    ImVec2 p_min = ImGui::GetCursorScreenPos();
                    float row_h = ImGui::GetTextLineHeightWithSpacing();
                    ImVec2 p_max(p_min.x + ImGui::GetContentRegionAvail().x,
                                 p_min.y + row_h);
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    dl->AddRectFilled(p_min, p_max,
                        IM_COL32(40, 40, 48, 255));
                }
            }

            bool is_sel = selection.contains(i);
            const char* ref_tag = (i == ref_idx) ? "Ref" : nullptr;

            if (is_sel) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.40f, 0.80f, 1.00f, 1.00f));
            }

            bool checked = is_sel;
            if (ImGui::Checkbox("##sel", &checked)) {
                if (in.group_by_name_ptr && *in.group_by_name_ptr && in.on_select_group) {
                    if (checked) {
                        in.on_select_group(i);
                    } else {
                        // Unselect the whole group.
                        std::string key =
                            group_key_from_filename(entry.filename);
                        for (int j = 0;
                             j < static_cast<int>(entries.size()); ++j) {
                            if (group_key_from_filename(
                                    entries[j].filename) == key) {
                                selection.erase(j);
                            }
                        }
                    }
                } else {
                    if (checked) {
                        selection.insert(i);
                    } else {
                        selection.erase(i);
                    }
                }
                // Selection change affects upscale targets for all selected images
                for (int s : selection.indices()) {
                    if (s >= 0 && s < static_cast<int>(entries.size())) {
                        entries[s].texture_dirty = true;
                    }
                }
                diff_service.mark_dirty();
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

            // Shift+click on the Selectable triggers range selection.
            // Plain click toggles the entry (or group) and updates the
            // anchor for future Shift+clicks.
            if (ImGui::IsItemHovered() &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                ImGuiIO& io = ImGui::GetIO();
                if (io.KeyShift && in.last_clicked_index &&
                    *in.last_clicked_index >= 0 &&
                    *in.last_clicked_index != i &&
                    in.on_select_range) {
                    in.on_select_range(*in.last_clicked_index, i);
                } else if (in.group_by_name_ptr && *in.group_by_name_ptr && in.on_select_group) {
                    in.on_select_group(i);
                    if (in.last_clicked_index)
                        *in.last_clicked_index = i;
                } else {
                    selection.toggle(i);
                    for (int s : selection.indices()) {
                        if (s >= 0 && s < static_cast<int>(entries.size()))
                            entries[s].texture_dirty = true;
                    }
                    diff_service.mark_dirty();
                    if (in.last_clicked_index)
                        *in.last_clicked_index = i;
                }
            }

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
                if (in.group_by_name_ptr && *in.group_by_name_ptr && in.on_select_group) {
                    if (ImGui::MenuItem("Select Group")) {
                        in.on_select_group(i);
                    }
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

    // Persist panel visibility if the close button was clicked.
    if (in.show_image_list && *in.show_image_list != was_visible) {
        if (in.on_settings_changed) in.on_settings_changed();
    }
}

} // namespace idiff
