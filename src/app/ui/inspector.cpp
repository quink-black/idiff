#include "app/ui/inspector.h"

#include "app/app.h"               // ImageEntry, Measurement
#include "app/metrics_panel.h"
#include "app/pixel_inspector_panel.h"
#include "app/properties_panel.h"
#include "app/viewport.h"
#include "core/image.h"
#include "domain/selection_model.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <string>
#include <utility>
#include <vector>

namespace idiff {

void render_right_sidebar(const InspectorInputs& in) {
    auto& entries = *in.entries;
    const auto& selection = *in.selection;

    ImGui::SetNextWindowSize(ImVec2(300, 400), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Inspector", in.show_inspector)) {
        ImGui::End();
        return;
    }

    // Resolve the reference index via the shared helper so the
    // inspector matches the viewport / image list labeling.  The first
    // partner (the smallest selected index that is not the reference)
    // feeds the side-by-side Properties / Metrics views below.
    int ref_idx = -1;
    if (in.get_ref_index) in.get_ref_index(ref_idx);
    int partner_idx = -1;
    for (int s : selection.indices()) {
        if (s == ref_idx) continue;
        partner_idx = s;
        break;
    }

    auto get_entry = [&](int idx) -> const ImageEntry* {
        if (idx < 0 || idx >= static_cast<int>(entries.size())) return nullptr;
        return &entries[idx];
    };
    const ImageEntry* entry_ref = get_entry(ref_idx);
    const ImageEntry* entry_partner = get_entry(partner_idx);

    const Image* img_ref = entry_ref ? entry_ref->image.get() : nullptr;
    const Image* img_partner = entry_partner ? entry_partner->image.get() : nullptr;
    const Image* disp_ref = entry_ref
        ? (entry_ref->display_image ? entry_ref->display_image.get()
                                    : entry_ref->image.get())
        : nullptr;
    const Image* disp_partner = entry_partner
        ? (entry_partner->display_image ? entry_partner->display_image.get()
                                        : entry_partner->image.get())
        : nullptr;
    const char* name_ref = entry_ref ? entry_ref->display_label.c_str() : nullptr;
    const char* name_partner = entry_partner ? entry_partner->display_label.c_str() : nullptr;

    // Sub-panel selection.  We keep a local mirror of *in.current_panel
    // when the host did not supply persistent storage so the user can
    // still switch tabs within a session, and avoids null deref below.
    static int s_local_panel = 0;
    int* current_panel = in.current_panel ? in.current_panel : &s_local_panel;
    // Out-of-range values (e.g. an old settings file written before a
    // panel was added or removed) clamp back to Properties so the
    // dropdown is not blank.
    if (*current_panel < 0 || *current_panel > 4) *current_panel = 0;

    // Single dropdown replaces the previous BeginTabBar so each
    // sub-panel gets the full vertical extent of the inspector
    // window -- crucial for dense tables like Pixel.
    int prev_panel = *current_panel;
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::Combo("##inspector_panel", current_panel,
                 "Properties\0Pixel\0Metrics\0Statistics\0Measurements\0\0");
    if (*current_panel != prev_panel && in.on_panel_changed) {
        in.on_panel_changed();
    }
    ImGui::Separator();

    switch (*current_panel) {
        case 0: { // Properties
            if (in.properties_panel) {
                in.properties_panel->render_inline(img_ref, img_partner,
                                                   disp_ref, disp_partner,
                                                   name_ref, name_partner);
            } else {
                ImGui::TextDisabled("Properties panel unavailable.");
            }
            break;
        }
        case 1: { // Pixel
            if (in.pixel_panel) {
                // Build the (label, image) list in viewport order:
                // reference first, then any other selected entries in
                // natural order.  This matches what the viewport draws
                // and what the existing Metrics tab uses.
                std::vector<std::pair<std::string, const Image*>> samples;
                auto add_entry_native = [&](int idx) {
                    if (idx < 0 ||
                        idx >= static_cast<int>(entries.size())) return;
                    const auto& e = entries[idx];
                    const Image* native = e.image.get();
                    if (!native) return;
                    samples.emplace_back(e.display_label, native);
                };
                add_entry_native(ref_idx);
                for (int s : selection.indices()) {
                    if (s == ref_idx) continue;
                    add_entry_native(s);
                }
                PixelInspectorInputs pix_in;
                pix_in.samples = std::move(samples);
                in.pixel_panel->render(pix_in);
            } else {
                ImGui::TextDisabled("Pixel panel unavailable.");
            }
            break;
        }
        case 2: { // Metrics
            if (in.metrics_panel) {
                // Multi-image metrics: compare the reference image
                // against every other selected entry.  Rows follow the
                // same order as the viewport cells and as
                // diff_service_->slots(), so the visual/spatial layout
                // and the metrics table stay in lockstep.
                std::vector<std::pair<std::string, const Image*>> partners;
                auto add_partner = [&](int idx) {
                    if (idx < 0 ||
                        idx >= static_cast<int>(entries.size())) return;
                    const auto& e = entries[idx];
                    const Image* disp = e.display_image
                                            ? e.display_image.get()
                                            : e.image.get();
                    if (!disp) return;
                    partners.emplace_back(e.display_label, disp);
                };
                for (int s : selection.indices()) {
                    if (s == ref_idx) continue;
                    add_partner(s);
                }
                in.metrics_panel->render_pair_metrics(disp_ref, partners);
            } else {
                ImGui::TextDisabled("Metrics panel unavailable.");
            }
            break;
        }
        case 3: { // Statistics
            if (in.metrics_panel) {
                // Gather all selected images with their names for per-image stats
                std::vector<std::pair<std::string, const Image*>> stat_images;

                // Reference first, then remaining selected (same order
                // as viewport).
                auto add_entry = [&](int idx, const char* prefix) {
                    if (idx < 0 || idx >= static_cast<int>(entries.size())) return;
                    const auto& e = entries[idx];
                    const Image* disp = e.display_image ? e.display_image.get()
                                                        : e.image.get();
                    if (!disp) return;
                    std::string label = prefix
                        ? (std::string("[") + prefix + "] " + e.display_label)
                        : e.display_label;
                    stat_images.emplace_back(std::move(label), disp);
                };

                add_entry(ref_idx, "Ref");
                for (int s : selection.indices()) {
                    if (s == ref_idx) continue;
                    add_entry(s, nullptr);
                }

                in.metrics_panel->render_statistics(stat_images);
            } else {
                ImGui::TextDisabled("Statistics panel unavailable.");
            }
            break;
        }
        case 4: { // Measurements
            if (in.properties_panel && in.viewport &&
                in.viewport_slot_to_entry) {
                // Build slot labels in the same order viewport_slot_to_entry
                // was populated by render_viewport, so
                // Measurement.source_cell_index resolves to the right image
                // name.  Keep the strings alive for the duration of the call.
                const auto& slot_to_entry = *in.viewport_slot_to_entry;
                std::vector<std::string> slot_storage;
                std::vector<const char*> slot_labels;
                slot_storage.reserve(slot_to_entry.size());
                slot_labels.reserve(slot_to_entry.size());
                for (int entry_idx : slot_to_entry) {
                    if (entry_idx >= 0 &&
                        entry_idx < static_cast<int>(entries.size())) {
                        slot_storage.push_back(entries[entry_idx].display_label);
                    } else {
                        slot_storage.emplace_back("(unknown)");
                    }
                    slot_labels.push_back(slot_storage.back().c_str());
                }
                std::vector<int> deleted_ids;
                bool clear_all = false;
                in.properties_panel->render_measurements(
                    *in.viewport, slot_labels, deleted_ids, clear_all);
                // Sync deletions back to the owning entries.
                if (clear_all) {
                    for (int entry_idx : slot_to_entry) {
                        if (entry_idx >= 0 &&
                            entry_idx < static_cast<int>(entries.size())) {
                            entries[entry_idx].measurements.clear();
                        }
                    }
                }
                for (int del_id : deleted_ids) {
                    for (auto& entry : entries) {
                        auto& ms = entry.measurements;
                        ms.erase(std::remove_if(ms.begin(), ms.end(),
                                                [del_id](const Measurement& m) {
                                                    return m.id == del_id;
                                                }),
                                 ms.end());
                    }
                }
            } else {
                ImGui::TextDisabled("Measurements panel unavailable.");
            }
            break;
        }
        default:
            break;
    }

    ImGui::End();
}

} // namespace idiff
