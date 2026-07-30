#include "app/ui/status_bar.h"

// The hover-pixel readout in this file intentionally stays single-image
// (the cell currently under the cursor).  Multi-image / multi-coordinate
// inspection lives in the Inspector -> Pixel sub-panel
// (PixelInspectorPanel); please do not stack additional pixel fields
// here -- they make the status bar unreadable on narrower windows.

#include "app/app.h"            // ImageEntry
#include "app/pixel_sampler.h"
#include "app/viewport.h"
#include "core/image.h"
#include "core/media_source.h"
#include "domain/diff_service.h"
#include "domain/selection_model.h"
#include "domain/timeline_model.h"

#include <imgui.h>
#include <opencv2/core.hpp>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>

namespace idiff {

void render_status_bar(const StatusBarInputs& in) {
    const auto& entries = *in.entries;
    const auto& selection = *in.selection;
    const auto& vport = *in.viewport;
    const auto& diff_service = *in.diff_service;
    const auto& slot_to_entry = *in.viewport_slot_to_entry;

    ImGuiViewport* vp = ImGui::GetMainViewport();

    // Height must accommodate the menu bar we draw inside.  Using the
    // current frame height (with spacing) keeps the bar aligned with the
    // font size / DPI scale, rather than hard-coding a pixel count that is
    // wrong at non-default scales.  MUST match the reservation made in
    // frame() so the DockSpace and the status bar tile exactly.
    float bar_h = ImGui::GetFrameHeightWithSpacing();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x,
                                    vp->WorkPos.y + vp->WorkSize.y - bar_h));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, bar_h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 4));

    // Without NoTitleBar the default ImGui title bar consumes the whole
    // 24-px slot and the MenuBar (where we actually draw the status text)
    // gets clipped to zero height, making the whole bar invisible.  Also
    // lock the window in place so users cannot accidentally move/resize it.
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                              ImGuiWindowFlags_NoResize |
                              ImGuiWindowFlags_NoMove |
                              ImGuiWindowFlags_NoCollapse |
                              ImGuiWindowFlags_NoDocking |
                              ImGuiWindowFlags_NoBringToFrontOnFocus |
                              ImGuiWindowFlags_NoNavFocus |
                              ImGuiWindowFlags_NoScrollbar |
                              ImGuiWindowFlags_NoSavedSettings |
                              ImGuiWindowFlags_MenuBar;

    if (ImGui::Begin("##statusbar", nullptr, flags)) {
        if (ImGui::BeginMenuBar()) {
            // Identity chip on the far left.  Lets the user tell
            // which idiff window (and which /tmp/idiff-*.sock) this
            // is.  Hovering reveals the full socket path so the user
            // can copy/paste it to MCP / scripts.  Background colour
            // matches the docking-preview accent so it stands out
            // from the rest of the menu bar without being noisy.
            if (in.identity_label && !in.identity_label->empty()) {
                ImVec4 chip_col(0.18f, 0.36f, 0.60f, 1.00f);
                ImGui::PushStyleColor(ImGuiCol_Button, chip_col);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, chip_col);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, chip_col);
                // SmallButton acts purely as a clickable label that
                // styles the way we want; no action on click.
                ImGui::SmallButton(in.identity_label->c_str());
                ImGui::PopStyleColor(3);
                if (ImGui::IsItemHovered()
                    && in.identity_socket_path
                    && !in.identity_socket_path->empty()) {
                    ImGui::SetTooltip("%s",
                                       in.identity_socket_path->c_str());
                }
                ImGui::Separator();
            }

            char buf[1024];
            int n = std::snprintf(buf, sizeof(buf), "%zu images | %zu selected",
                                   entries.size(), selection.size());
            auto append = [&](const char* fmt, ...) {
                if (n < 0 || static_cast<size_t>(n) >= sizeof(buf)) return;
                va_list ap;
                va_start(ap, fmt);
                int m = std::vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
                va_end(ap);
                if (m > 0) n += m;
            };

            int ref_idx = -1;
            in.get_ref_index(ref_idx);
            if (ref_idx >= 0 && ref_idx < static_cast<int>(entries.size())) {
                append(" | Ref: %s", entries[ref_idx].display_label.c_str());
            }
            int extra = static_cast<int>(selection.size()) -
                        (ref_idx >= 0 ? 1 : 0);
            if (extra > 0) {
                append(" (+%d partner%s)", extra, extra == 1 ? "" : "s");
            }

            // Hover pixel readout -- resolved against the display image
            // that the viewport actually drew (so coordinates match
            // what's on screen, even when one image was upscaled to
            // match the other).
            if (vport.hover_valid()) {
                int cell = vport.hover_cell_index();
                int px = vport.hover_pixel_x();
                int py = vport.hover_pixel_y();

                const char* src_label = nullptr;
                const Image* src_img = nullptr;
                // Diff heatmaps are already at display resolution, so
                // their mat dims are the correct normalizer.  Every
                // other mode passes 0/0 to let hover_pixel_to_norm pick
                // the image's display_width/height.
                int override_disp_w = 0;
                int override_disp_h = 0;

                if (vport.mode() == ComparisonMode::Difference) {
                    // Map the hovered cell back to its diff slot so
                    // the status bar shows "Ref vs <partner>" and the
                    // pixel value comes from the heatmap the user is
                    // looking at.
                    if (cell >= 0 &&
                        cell < static_cast<int>(diff_service.size())) {
                        const auto& slot = diff_service.slots()[cell];
                        src_img = slot.image.get();
                        if (src_img && !src_img->mat().empty()) {
                            override_disp_w = src_img->mat().cols;
                            override_disp_h = src_img->mat().rows;
                        }
                        static thread_local std::string diff_label;
                        std::string partner = "?";
                        if (slot.partner_entry_idx >= 0 &&
                            slot.partner_entry_idx <
                                static_cast<int>(entries.size())) {
                            partner =
                                entries[slot.partner_entry_idx].display_label;
                        }
                        int ref_idx = -1;
                        in.get_ref_index(ref_idx);
                        std::string ref_name = (ref_idx >= 0 &&
                                                ref_idx < static_cast<int>(entries.size()))
                                                  ? entries[ref_idx].display_label
                                                  : std::string("Ref");
                        diff_label = "Diff: " + ref_name + " vs " + partner;
                        src_label = diff_label.c_str();
                    } else {
                        src_label = "Diff";
                    }
                } else if (cell >= 0 &&
                           cell < static_cast<int>(slot_to_entry.size())) {
                    int ent = slot_to_entry[cell];
                    if (ent >= 0 && ent < static_cast<int>(entries.size())) {
                        const auto& e = entries[ent];
                        // Sample from the original image rather than
                        // display_image, which may be upscaled with
                        // interpolated pixels.
                        src_img = e.image.get();
                        src_label = e.display_label.c_str();
                    }
                }

                HoverNorm hn = hover_pixel_to_norm(src_img, px, py,
                                                   override_disp_w,
                                                   override_disp_h);
                if (hn.valid) {
                    const auto& m = src_img->mat();
                    // Show source-native coordinates so the readout
                    // matches the pixel inspector's (x, y) column and
                    // stays within the source frame's bounds.
                    int sx = (!m.empty()) ? norm_to_pixel(hn.u, m.cols) : px;
                    int sy = (!m.empty()) ? norm_to_pixel(hn.v, m.rows) : py;
                    append(" | %s @ (%d, %d)",
                           src_label ? src_label : "?", sx, sy);

                    if (!m.empty()) {
                        // Status bar always reflects what is on screen:
                        // the post-conversion 8-bit sRGB pixel.  Force
                        // the RGB path so the inspector's YUV/RGB
                        // toggle does not silently shift the meaning
                        // of these numbers.  format_pixel adds the
                        // "R G B:" prefix so users no longer have to
                        // guess what (a, b, c) means.
                        PixelSample s = sample_image_at(src_img, hn.u, hn.v,
                                                        /*prefer_rgb=*/true);
                        char vbuf[96];
                        if (s.valid && format_pixel(s, vbuf, sizeof(vbuf))) {
                            append(" = %s", vbuf);
                        }
                    }
                } else if (src_label) {
                    append(" | %s", src_label);
                }
            }

            if (!in.status_text->empty()) {
                append(" | %s", in.status_text->c_str());
            }

            ImGui::TextUnformatted(buf);
            ImGui::EndMenuBar();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

float render_timeline_bar(const TimelineBarInputs& in) {
    auto& entries = *in.entries;
    auto& timeline = *in.timeline;
    const int length = TimelineModel::length(entries);
    if (length <= 1) return 0.0f;

    // Clamp the shared index once per frame so any external mutation
    // (e.g. adding / removing entries) can't leave it past the new end.
    timeline.clamp_to_length(entries);

    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float row_h = ImGui::GetFrameHeightWithSpacing();

    int offset_rows = 0;
    for (const auto& e : entries) {
        if (e.source && e.source->frame_count() > 1) offset_rows++;
    }
    const int visible_offset_rows = std::min(offset_rows, 4);
    const float bar_h = row_h * (1.0f + visible_offset_rows) + 8.0f;

    const float status_bar_h = ImGui::GetFrameHeightWithSpacing();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x,
                                    vp->WorkPos.y + vp->WorkSize.y
                                    - status_bar_h - bar_h));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, bar_h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 4));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                              ImGuiWindowFlags_NoResize |
                              ImGuiWindowFlags_NoMove |
                              ImGuiWindowFlags_NoCollapse |
                              ImGuiWindowFlags_NoDocking |
                              ImGuiWindowFlags_NoBringToFrontOnFocus |
                              ImGuiWindowFlags_NoNavFocus |
                              ImGuiWindowFlags_NoSavedSettings;

    bool frame_changed = false;

    if (ImGui::Begin("##timeline", nullptr, flags)) {
        if (ImGui::SmallButton("<")) {
            if (timeline.current_frame() > 0) {
                timeline.set_current_frame(timeline.current_frame() - 1);
                frame_changed = true;
            }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(">")) {
            if (timeline.current_frame() < length - 1) {
                timeline.set_current_frame(timeline.current_frame() + 1);
                frame_changed = true;
            }
        }
        ImGui::SameLine();

        int frame = timeline.current_frame();
        ImGui::SetNextItemWidth(-200.0f);
        if (ImGui::SliderInt("##frame", &frame, 0, length - 1, "Frame %d")) {
            timeline.set_current_frame(frame);
        }
        // During drag: fast keyframe preview for scrub feedback.
        // On release: exact frame decode via on_frame_changed.
        if (ImGui::IsItemActive() && in.on_frame_preview) {
            in.on_frame_preview();
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            frame_changed = true;
        }
        ImGui::SameLine();
        ImGui::Text("of %d", length);

        if (offset_rows > 0) {
            ImGui::BeginChild("##offsets",
                              ImVec2(0, row_h * visible_offset_rows),
                              false);
            for (std::size_t i = 0; i < entries.size(); ++i) {
                auto& e = entries[i];
                if (!e.source || e.source->frame_count() <= 1) continue;

                int effective = timeline.current_frame() + e.frame_offset;
                if (effective < 0) effective = 0;
                int cnt = e.source->frame_count();
                if (effective >= cnt) effective = cnt - 1;

                ImGui::PushID(static_cast<int>(i));
                ImGui::TextUnformatted(e.filename.c_str());
                ImGui::SameLine();
                ImGui::SetNextItemWidth(160.0f);
                int off = e.frame_offset;
                if (ImGui::InputInt("offset", &off, 1, 10)) {
                    if (off != e.frame_offset) {
                        e.frame_offset = off;
                        frame_changed = true;
                    }
                }
                ImGui::SameLine();
                ImGui::TextDisabled("-> frame %d / %d", effective, cnt - 1);
                ImGui::PopID();
            }
            ImGui::EndChild();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();

    if (frame_changed && in.on_frame_changed) {
        in.on_frame_changed();
    }

    return bar_h;
}

} // namespace idiff
