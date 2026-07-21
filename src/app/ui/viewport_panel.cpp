#include "app/ui/viewport_panel.h"

#include "app/app.h"          // ImageEntry, Measurement
#include "app/settings.h"     // AppSettings
#include "app/viewport.h"
#include "core/image.h"
#include "domain/diff_service.h"
#include "domain/selection_model.h"

#include <imgui.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace idiff {

void render_viewport_panel(const ViewportPanelInputs& in) {
    auto& entries = *in.entries;
    auto& selection = *in.selection;
    auto& diff_service = *in.diff_service;
    auto& settings = *in.settings;
    auto& slot_to_entry = *in.viewport_slot_to_entry;

    // Snapshot the diff service's dirty flag at the top of the frame:
    // diff_service.update() below clears it, but the measurement
    // reload path further down still needs to know whether the image
    // set or selection just changed.
    bool selection_changed = diff_service.is_dirty();

    // When the image set or selection changes, clear the viewport's
    // measurement display and reload from the new entries.  Measurements
    // are persisted per-entry and synced immediately on create/delete,
    // so no "save back" step is needed here.
    if (selection_changed && in.viewport) {
        in.viewport->clear_measurements();
    }

    // Release decoded pixels for entries that just left the selection
    // so they don't sit idle in memory.
    if (selection_changed && in.on_evict_non_selected) {
        in.on_evict_non_selected();
    }

    // Upload dirty textures for selected images
    for (int s : selection.indices()) {
        if (s >= 0 && s < static_cast<int>(entries.size())) {
            if (entries[s].texture_dirty) {
                if (in.on_update_display_image) in.on_update_display_image(s);
                if (in.on_upload_texture) in.on_upload_texture(s);
            }
        }
    }

    {
        DiffService::Options opts;
        opts.amplification = *in.diff_amplification;
        opts.heatmap_color = *in.heatmap_color;
        opts.channel_mode = in.viewport->channel_view_mode();
        diff_service.update(entries, selection, opts, *in.status_text);
    }

    // Build texture list from selected images. Place the reference
    // image first, followed by any additional selected images in their
    // natural order.
    std::vector<SDL_Texture*> tex_ptrs;
    std::vector<int> tex_ws, tex_hs;
    std::vector<const char*> labels;
    // Hold label storage so const char* remains valid for the frame
    std::vector<std::string> label_storage;
    label_storage.reserve(selection.size());

    // Reset the slot->entry mapping; repopulated below in lockstep with the
    // vectors above so render_status_bar can map hovered slots back.
    slot_to_entry.clear();
    slot_to_entry.reserve(selection.size());

    int ref_idx = -1;
    if (in.get_ref_index) in.get_ref_index(ref_idx);

    auto push_entry = [&](int s, const char* prefix) {
        if (s < 0 || s >= static_cast<int>(entries.size())) return;
        const auto& e = entries[s];
        tex_ptrs.push_back(e.texture);
        // Report the source image's display dimensions (SAR-adjusted),
        // not the SDL texture size.  When two selected images differ in
        // resolution, update_display_image upscales the smaller one to
        // max(display_w, display_h) for pixel-aligned diffing, which
        // inflates entry.tex_w/tex_h.  Using those inflated values would
        // make rulers and measurements report sizes in the upscaled
        // coordinate system.  Display dimensions give the correct visual
        // size for aspect-ratio-correct rendering.
        int src_w = e.image_decoded && e.image ? e.image->info().display_width()  : e.cached_info.display_width()  ? e.cached_info.display_width()  : e.tex_w;
        int src_h = e.image_decoded && e.image ? e.image->info().display_height() : e.cached_info.display_height() ? e.cached_info.display_height() : e.tex_h;
        tex_ws.push_back(src_w);
        tex_hs.push_back(src_h);
        // The slot index (the position of this cell in the viewport's
        // grid) is shown as a small bracketed prefix so the user has
        // a stable identifier they can quote when collaborating with
        // an AI agent over MCP -- "look at panel [1]" works without
        // ambiguity even when the underlying file path is long.
        const std::size_t slot_no = tex_ptrs.size() - 1;
        std::string lbl = prefix
            ? ("[" + std::to_string(slot_no) + "][" + prefix + "] " + e.display_label)
            : ("[" + std::to_string(slot_no) + "] " + e.display_label);
        label_storage.push_back(std::move(lbl));
        labels.push_back(label_storage.back().c_str());
        slot_to_entry.push_back(s);
    };

    if (ref_idx >= 0) push_entry(ref_idx, "Ref");
    for (int s : selection.indices()) {
        if (s == ref_idx) continue;
        push_entry(s, nullptr);
    }

    // Load saved measurements from the newly-mapped entries into the
    // viewport.  source_cell_index is rewritten to the current slot
    // position so the viewport can project the rectangles correctly.
    if (selection_changed && in.viewport) {
        auto& vp = *in.viewport;
        int max_id = 1;
        for (int slot = 0; slot < static_cast<int>(slot_to_entry.size()); slot++) {
            int entry_idx = slot_to_entry[slot];
            if (entry_idx >= 0 && entry_idx < static_cast<int>(entries.size())) {
                for (const auto& m : entries[entry_idx].measurements) {
                    Measurement copy = m;
                    copy.source_cell_index = slot;
                    vp.add_measurement(copy);
                }
                max_id = std::max(max_id, entries[entry_idx].next_measurement_id);
            }
        }
        vp.set_next_measurement_id(max_id);
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
    if (!ImGui::Begin("Viewport", nullptr,
                       ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }

    auto& vp = *in.viewport;
    ImGuiIO& io = ImGui::GetIO();
    bool hovered = ImGui::IsWindowHovered();
    bool focused = ImGui::IsWindowFocused();

    // --- Mouse wheel zoom with anchor at cursor position ---
    if (hovered) {
        float wheel = io.MouseWheel;
        if (wheel != 0.0f) {
            float factor = wheel > 0 ? 1.15f : (1.0f / 1.15f);
            float new_zoom = vp.zoom() * factor;
            vp.zoom_around(new_zoom, io.MousePos);
        }
    }

    // --- Middle-mouse drag to pan ---
    // Suppressed while a right-click selection rectangle is being drawn so
    // the two gestures never overlap.
    if (hovered && !vp.selecting() &&
        ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
        ImVec2 delta = io.MouseDelta;
        vp.set_pan(vp.pan_x() + delta.x, vp.pan_y() + delta.y);
    }

    // --- Left-mouse drag to pan ---
    // In Overlay mode, the InvisibleButton over the viewport area captures
    // left-mouse drags for the A/B slider.  Only pan when that slider is
    // NOT being dragged, no selection is in progress, and Ctrl is NOT held
    // (Ctrl+left-drag is selection-zoom), so all interactions remain
    // mutually exclusive.  Also skip when Measure mode is active: the
    // left-drag is handled by the measurement block below.
    if (hovered && !vp.selecting() && !io.KeyCtrl &&
        !vp.measure_mode() && !vp.measuring() &&
        ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        bool overlay_slider_active = vp.overlay_slider_dragging();
        if (!overlay_slider_active) {
            ImVec2 delta = io.MouseDelta;
            vp.set_pan(vp.pan_x() + delta.x, vp.pan_y() + delta.y);
        }
    }

    // --- Measurement drag (Measure mode, left button) ---
    // Mutually exclusive with pan and with selection-zoom.  The drag is
    // anchored on mouse-down at the cell under the cursor and that cell's
    // source-tex dimensions are frozen for the lifetime of the drag, so
    // dragging past the cell edge extrapolates in the source image's
    // coordinate system instead of re-resolving to another cell.
    //
    // Measure mode is a hold-to-activate hotkey: while M is held,
    // the next left-drag is captured as a measurement; releasing M
    // mid-drag does NOT abort, so the user can let go of the key
    // once dragging starts.  This is intentionally a bare letter --
    // App::frame() now keeps SDL_StopTextInput() in effect whenever
    // no ImGui InputText is focused, so the OS IME (Pinyin and the
    // like) never gets a chance to swallow M into a composition.
    // WantTextInput, not WantCaptureKeyboard, is the correct guard:
    // WantCaptureKeyboard is true whenever any ImGui window is
    // focused (including the viewport itself).
    bool measure_armed = !io.WantTextInput &&
                         ImGui::IsKeyDown(ImGuiKey_M);
    if (measure_armed != vp.measure_mode()) {
        vp.set_measure_mode(measure_armed);
        if (!measure_armed && !vp.measuring()) {
            vp.cancel_measurement();
        }
    }
    if (vp.measure_mode() || vp.measuring()) {
        bool click_left = hovered && !io.KeyCtrl &&
                          ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        // Clicking directly on an existing measurement's x button removes
        // it; do not also start a new drag from that click position.
        if (click_left && !vp.hover_measurement_close_hot() && !vp.selecting()) {
            vp.begin_measurement(io.MousePos);
        }
        if (vp.measuring()) {
            vp.update_measurement(io.MousePos);
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                const Measurement* committed = vp.end_measurement();
                // Sync the new measurement to its source entry so it
                // persists across selection changes.
                if (committed) {
                    int slot = committed->source_cell_index;
                    if (slot >= 0 && slot < static_cast<int>(slot_to_entry.size())) {
                        int entry_idx = slot_to_entry[slot];
                        if (entry_idx >= 0 && entry_idx < static_cast<int>(entries.size())) {
                            entries[entry_idx].measurements.push_back(*committed);
                            entries[entry_idx].next_measurement_id =
                                std::max(entries[entry_idx].next_measurement_id,
                                         committed->id + 1);
                        }
                    }
                }
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                vp.cancel_measurement();
            }
        }
    }

    // --- Selection rectangle zoom (right-drag OR Ctrl+left-drag) ---
    // Ctrl+left-drag gives trackpad users a way to zoom-to-selection on
    // macOS where right-click drag is not naturally available.
    bool sel_start_right = ImGui::IsMouseClicked(ImGuiMouseButton_Right) && hovered;
    bool sel_start_ctrl  = ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hovered && io.KeyCtrl;
    // --- Shift+left-click "quick pin" for the pixel inspector ---
    // Pure click (no drag) so it does not interfere with pan, measure,
    // or selection-zoom drags above.  KeyShift is otherwise unused by
    // the viewport, and we further gate on KeyCtrl / measure / sel-zoom
    // so a future feature that claims Shift+drag stays compatible.
    if (hovered && io.KeyShift && !io.KeyCtrl && !vp.measure_mode() &&
        !vp.measuring() && !vp.selecting() &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (in.on_shift_pin_click) in.on_shift_pin_click();
    }
    // P key: pin the pixel under the cursor without leaving the
    // viewport.  Bare letter for one-handed use, made safe by the
    // app-wide SDL_StopTextInput discipline: while no InputText is
    // focused, the IME never sees the keystroke, so this never
    // pops a candidate window even with Pinyin active.  Guard with
    // !KeyCtrl/!KeyShift/!KeyAlt so future modifier combinations
    // (e.g. Shift+P) stay free, and only fire one pin per key-down.
    if (hovered && !io.WantTextInput && !io.KeyCtrl && !io.KeyShift &&
        !io.KeyAlt && !vp.measure_mode() && !vp.measuring() &&
        !vp.selecting() &&
        ImGui::IsKeyPressed(ImGuiKey_P, /*repeat=*/false)) {
        if (in.on_shift_pin_click) in.on_shift_pin_click();
    }
    if (hovered || vp.selecting()) {
        if (!vp.selecting() && (sel_start_right || sel_start_ctrl)) {
            vp.begin_selection(io.MousePos);
            *in.sel_drag_is_ctrl = sel_start_ctrl;
        }
        if (vp.selecting()) {
            vp.update_selection(io.MousePos);
            bool released = *in.sel_drag_is_ctrl
                ? ImGui::IsMouseReleased(ImGuiMouseButton_Left)
                : ImGui::IsMouseReleased(ImGuiMouseButton_Right);
            if (released) {
                ImVec2 smin = vp.selection_min();
                ImVec2 smax = vp.selection_max();
                float sw = smax.x - smin.x;
                float sh = smax.y - smin.y;
                if (sw > 8.0f && sh > 8.0f) {
                    vp.end_selection();  // commits zoom_to_rect
                } else {
                    vp.cancel_selection();  // too small, treat as click
                }
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                vp.cancel_selection();
            }
        }
    }

    // --- Keyboard shortcuts (when viewport is focused) ---
    if (focused || hovered) {
        bool ctrl = io.KeyCtrl;
        // '0' or Ctrl+0 : fit to content
        if (ImGui::IsKeyPressed(ImGuiKey_0) || ImGui::IsKeyPressed(ImGuiKey_Keypad0)) {
            vp.fit_to_content();
        }
        // 'F' : fit to content
        if (ImGui::IsKeyPressed(ImGuiKey_F) && !ctrl) {
            vp.fit_to_content();
        }
        // Number keys for channel view are handled in the global shortcut
        // block above so they work even when the Viewport is not focused.
    }

    // --- Double-click to fit ---
    // Disabled in Measure mode: left-clicks there belong to the
    // measurement drag interaction and a surprise fit would wipe the
    // context the user is trying to measure in.
    if (hovered && !vp.measure_mode() &&
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        vp.fit_to_content();
    }

    // Toolbar
    {
        ComparisonMode mode = vp.mode();
        int mode_int = static_cast<int>(mode);
        ImGui::RadioButton("Split", &mode_int, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Overlay", &mode_int, 1);
        ImGui::SameLine();
        ImGui::RadioButton("Diff", &mode_int, 2);
        if (static_cast<ComparisonMode>(mode_int) != mode) {
            vp.set_mode(static_cast<ComparisonMode>(mode_int));
            if (in.settings) in.settings->save();
        }

        // Grid layout selector (only meaningful in Split/Diff modes)
        ComparisonMode current_mode = static_cast<ComparisonMode>(mode_int);
        if (current_mode == ComparisonMode::Split || current_mode == ComparisonMode::Difference) {
            ImGui::SameLine();
            ImGui::Spacing();
            ImGui::SameLine();
            ImGui::Text("Layout:");
            ImGui::SameLine();
            GridLayout gl = vp.grid_layout();
            int gl_int = static_cast<int>(gl);
            ImGui::PushItemWidth(70);
            if (ImGui::Combo("##layout", &gl_int, "Auto\0Row\0Col\0NxM\0")) {
                gl = static_cast<GridLayout>(gl_int);
                vp.set_grid_layout(gl);
                settings.grid_layout = gl_int;
                if (in.on_settings_changed) in.on_settings_changed();
            }
            ImGui::PopItemWidth();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Grid layout: Auto (heuristic), Row (1xN), Col (Nx1), NxM (custom columns)");
            }

            if (gl == GridLayout::RowsCols) {
                ImGui::SameLine();
                int cols = vp.grid_cols();
                ImGui::PushItemWidth(40);
                if (ImGui::InputInt("C##grid_cols", &cols, 1, 1)) {
                    vp.set_grid_cols(cols);
                    settings.grid_cols = vp.grid_cols();
                    if (in.on_settings_changed) in.on_settings_changed();
                }
                ImGui::PopItemWidth();
            }
        }

        // Difference-mode options (heatmap color + amplification)
        if (current_mode == ComparisonMode::Difference) {
            ImGui::SameLine();
            ImGui::Spacing();
            ImGui::SameLine();
            ImGui::Text("Heatmap:");
            ImGui::SameLine();
            int hc_int = static_cast<int>(*in.heatmap_color);
            ImGui::PushItemWidth(70);
            if (ImGui::Combo("##heatmap_color", &hc_int, "Gray\0Inferno\0Viridis\0Coolwarm\0")) {
                *in.heatmap_color = static_cast<HeatmapColor>(hc_int);
                diff_service.mark_dirty();
                settings.heatmap_color = hc_int;
                if (in.on_settings_changed) in.on_settings_changed();
            }
            ImGui::PopItemWidth();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Heatmap color scheme");
            }

            ImGui::SameLine();
            float amp = static_cast<float>(*in.diff_amplification);
            ImGui::PushItemWidth(60);
            if (ImGui::SliderFloat("Amp##diff_amp", &amp, 1.0f, 50.0f, "%.1fx")) {
                *in.diff_amplification = static_cast<double>(amp);
                diff_service.mark_dirty();
                settings.diff_amplification = *in.diff_amplification;
                if (in.on_settings_changed) in.on_settings_changed();
            }
            ImGui::PopItemWidth();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Difference amplification factor");
            }
        }

        ImGui::SameLine();
        ImGui::Spacing();
        ImGui::SameLine();

        float zoom = vp.zoom();
        if (ImGui::SmallButton("-")) {
            // Zoom out centered on viewport
            ImVec2 vp_center(vp.viewport_origin().x + vp.viewport_size().x * 0.5f,
                             vp.viewport_origin().y + vp.viewport_size().y * 0.5f);
            vp.zoom_around(zoom * 0.8f, vp_center);
        }
        ImGui::SameLine();
        ImGui::Text("%.0f%%", vp.zoom() * 100.0f);
        ImGui::SameLine();
        if (ImGui::SmallButton("+")) {
            ImVec2 vp_center(vp.viewport_origin().x + vp.viewport_size().x * 0.5f,
                             vp.viewport_origin().y + vp.viewport_size().y * 0.5f);
            vp.zoom_around(zoom * 1.25f, vp_center);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Fit")) {
            vp.fit_to_content();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("1:1")) {
            vp.zoom_to_actual();
        }

        ImGui::SameLine();
        ImGui::Spacing();
        ImGui::SameLine();
        bool can_save = !entries.empty() &&
                        (!selection.empty() ||
                         (vp.mode() == ComparisonMode::Difference && !diff_service.empty()));
        ImGui::BeginDisabled(!can_save);
        if (ImGui::SmallButton("Save...")) {
            if (in.on_save_viewport) in.on_save_viewport();
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Save the current viewport (Split / Overlay / Diff) "
                              "to a PNG or JPEG file");
        }

        ImGui::SameLine();
        ImGui::Spacing();
        ImGui::SameLine();

        {
            bool ruler = vp.show_ruler();
            if (ImGui::Checkbox("Ruler", &ruler)) {
                vp.set_show_ruler(ruler);
                settings.show_ruler = ruler;
                if (in.on_settings_changed) in.on_settings_changed();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Show coordinate rulers along image edges");
            }
        }
        ImGui::SameLine();
        {
            bool grid = vp.show_grid();
            if (ImGui::Checkbox("Grid", &grid)) {
                vp.set_show_grid(grid);
                settings.show_grid = grid;
                if (in.on_settings_changed) in.on_settings_changed();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Show grid overlay on images");
            }
        }

        // --- Measurement mode ---
        // Measurement is a hold-to-activate gesture: hold M and left-drag
        // a region.  No persistent checkbox in the toolbar -- a sticky
        // toggle was too easy to leave on, causing stray clicks to draw
        // unwanted measurement rectangles.
        if (!vp.measurements().empty()) {
            ImGui::SameLine();
            ImGui::Spacing();
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear Measurements")) {
                vp.clear_measurements();
                // Also clear from the owning entries so they don't
                // reappear on the next selection change.
                for (int entry_idx : slot_to_entry) {
                    if (entry_idx >= 0 && entry_idx < static_cast<int>(entries.size())) {
                        entries[entry_idx].measurements.clear();
                    }
                }
            }
        if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Remove all saved measurements");
            }
        }

        int sel_count = static_cast<int>(selection.size());
        if (sel_count > 0) {
            ImGui::SameLine();
            ImGui::Spacing();
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.40f, 0.80f, 1.00f, 1.00f),
                               "%d images", sel_count);
        }

        ImGui::Separator();

        // Hint about mouse interactions
        if (vp.measure_mode()) {
            ImGui::TextColored(ImVec4(0xFF/255.0f, 0xC8/255.0f, 0x32/255.0f, 1.00f),
                               "Measure (M held): left-drag a region | "
                               "Right-drag / Ctrl+drag: zoom to selection | "
                               "Hover a rect and click x to remove");
        } else {
            ImGui::TextColored(ImVec4(0.60f, 0.60f, 0.60f, 1.00f),
                               "Drag: pan | Right-drag / Ctrl+drag: zoom to selection | "
                               "Hold M + drag: measure region");
        }
    }

    // Build diff texture/label vectors for Difference mode.  Each slot
    // is "A vs <partner>", in the same order DiffService::update()
    // populates the slot list, so metrics rows and viewport cells align.
    std::vector<SDL_Texture*> diff_tex_ptrs;
    std::vector<int> diff_tex_ws, diff_tex_hs;
    std::vector<const char*> diff_labels;
    std::vector<std::string> diff_label_storage;
    diff_tex_ptrs.reserve(diff_service.size());
    diff_tex_ws.reserve(diff_service.size());
    diff_tex_hs.reserve(diff_service.size());
    diff_labels.reserve(diff_service.size());
    diff_label_storage.reserve(diff_service.size());
    {
        int ref_lbl_idx = -1;
        if (in.get_ref_index) in.get_ref_index(ref_lbl_idx);
        std::string ref_name = (ref_lbl_idx >= 0 &&
                                ref_lbl_idx < static_cast<int>(entries.size()))
                                    ? entries[ref_lbl_idx].display_label
                                    : std::string("Ref");
        for (const auto& slot : diff_service.slots()) {
            diff_tex_ptrs.push_back(slot.texture);
            diff_tex_ws.push_back(slot.tex_w);
            diff_tex_hs.push_back(slot.tex_h);
            std::string partner_name;
            if (slot.partner_entry_idx >= 0 &&
                slot.partner_entry_idx < static_cast<int>(entries.size())) {
                partner_name = entries[slot.partner_entry_idx].display_label;
            } else {
                partner_name = "?";
            }
            diff_label_storage.push_back("Diff: " + ref_name + " vs " + partner_name);
            diff_labels.push_back(diff_label_storage.back().c_str());
        }
    }

    // Render viewport content
    vp.render(tex_ptrs, tex_ws, tex_hs, labels,
              diff_tex_ptrs, diff_tex_ws, diff_tex_hs, diff_labels);

    // Detect channel view mode changes triggered inside the Viewport
    // combo and mark selected entries dirty so textures are re-uploaded
    // with the new channel extraction on the next frame.
    if (vp.channel_view_mode() != *in.last_channel_view_mode) {
        *in.last_channel_view_mode = vp.channel_view_mode();
        for (int s : selection.indices()) {
            if (s >= 0 && s < static_cast<int>(entries.size())) {
                entries[s].texture_dirty = true;
            }
        }
        // The diff must also recompute so the heatmap reflects the
        // newly-selected channel instead of all-RGB.
        diff_service.mark_dirty();
    }

    ImGui::End();
    ImGui::PopStyleVar();

    // Sync measurement deletions from the viewport (x button clicks
    // processed during draw_measurements) back to the owning entries.
    int deleted_id;
    while ((deleted_id = vp.consume_pending_delete()) >= 0) {
        for (auto& entry : entries) {
            auto& ms = entry.measurements;
            ms.erase(std::remove_if(ms.begin(), ms.end(),
                                    [deleted_id](const Measurement& m) {
                                        return m.id == deleted_id;
                                    }),
                     ms.end());
        }
    }
}

} // namespace idiff
