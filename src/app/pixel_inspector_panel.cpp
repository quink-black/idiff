#include "app/pixel_inspector_panel.h"

#include "app/pixel_sampler.h"
#include "core/image.h"

#include <imgui.h>
#include <opencv2/core.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace idiff {

namespace {

// Clamp a double into the half-open [0, 1) range used by the panel.
// Values >= 1.0 collapse to nextafter(1.0, 0.0) so sample_image still
// sees an in-range coord; negative or NaN inputs become 0.
double clamp_unit(double x) {
    if (!(x >= 0.0)) return 0.0;
    if (x >= 1.0)   return std::nextafter(1.0, 0.0);
    return x;
}

// "—" em-dash, UTF-8.  Matches what pixel_sampler emits on invalid.
constexpr const char* kEmDash = "\xE2\x80\x94";

// Pick the reference resolution used to (a) display integer (x, y)
// columns and (b) translate integer-edit input back to normalized.
// Preference order: first sample (A) -> first sample with a non-empty
// image -> (-1, -1) marker telling the caller to hide the integer edit.
struct ReferenceRes {
    int w = -1;
    int h = -1;
    std::string label;
};

ReferenceRes pick_reference(
    const std::vector<std::pair<std::string, const Image*>>& samples) {
    ReferenceRes r;
    if (samples.empty()) return r;
    // A is samples[0] by contract; only fall back if A is missing pixel
    // data entirely.
    for (size_t i = 0; i < samples.size(); ++i) {
        const Image* img = samples[i].second;
        if (img && !img->mat().empty()) {
            r.w = img->mat().cols;
            r.h = img->mat().rows;
            r.label = samples[i].first;
            return r;
        }
    }
    return r;
}

// Build a one-shot row label like "  [A] foo.png".  Per-row prefix
// makes A obvious without forcing a separate column.
std::string format_row_label(int sample_idx, const std::string& name) {
    if (sample_idx == 0) return std::string("[A] ") + name;
    if (sample_idx == 1) return std::string("[B] ") + name;
    return name;
}

} // namespace

PixelInspectorPanel::PixelInspectorPanel() = default;
PixelInspectorPanel::~PixelInspectorPanel() = default;

void PixelInspectorPanel::update_hover(double u, double v, bool valid) {
    hover_valid_ = valid;
    if (valid) {
        hover_.u = clamp_unit(u);
        hover_.v = clamp_unit(v);
    }
}

void PixelInspectorPanel::pin_current_hover() {
    if (!hover_valid_) return;
    PixelSamplePoint p;
    p.u = hover_.u;
    p.v = hover_.v;
    pinned_.push_back(p);
}

void PixelInspectorPanel::pin_at(double u, double v) {
    PixelSamplePoint p;
    p.u = clamp_unit(u);
    p.v = clamp_unit(v);
    pinned_.push_back(p);
}

void PixelInspectorPanel::clear_pins() {
    pinned_.clear();
}

void PixelInspectorPanel::render(const PixelInspectorInputs& inputs) {
    const auto& samples = inputs.samples;

    // --- Header: reference + global actions -----------------------------
    // Reference resolution.  Kept short ("Ref:") so it fits on a single
    // line even when the inspector pane is narrow -- the spelling-out
    // "Reference: A = ..." used to push the row off-screen.
    ReferenceRes ref = pick_reference(samples);
    if (ref.w > 0 && ref.h > 0) {
        ImGui::Text("Ref: %s %dx%d",
                    ref.label.c_str(), ref.w, ref.h);
    } else {
        ImGui::TextDisabled("No reference image");
    }

    // --- Direct coordinate entry + clear ---------------------------------
    // Single compact toolbar: "x,y [InputInt2] [Add]   [Clear]".
    // The "Pin last hover" button that used to live here has been
    // removed: by the time the user moves the cursor from the image to
    // a button on the inspector, hover_valid_ has already flipped to
    // false (or, worse, latched onto a neighbouring image cell on the
    // way over).  Pinning the hover is now the job of the viewport
    // hotkey (Alt+P) and Shift+Click, which only fire while the
    // cursor is still over the intended pixel.
    {
        static int s_input_xy[2] = {0, 0};
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("x,y");
        ImGui::SameLine();
        // 90px is enough for two 4-digit ints; the spinner step
        // buttons are absent because InputInt2 hides them by design,
        // which keeps the widget narrow.
        ImGui::SetNextItemWidth(90.0f);
        ImGui::InputInt2("##add_xy", s_input_xy);
        ImGui::SameLine();
        bool add_enabled = !samples.empty();
        ImGui::BeginDisabled(!add_enabled);
        bool add_clicked = ImGui::SmallButton("Add");
        ImGui::EndDisabled();
        if (add_clicked) {
            int rw = (ref.w > 0) ? ref.w : 1;
            int rh = (ref.h > 0) ? ref.h : 1;
            int x = s_input_xy[0];
            int y = s_input_xy[1];
            if (x < 0) x = 0;
            if (y < 0) y = 0;
            if (x >= rw) x = rw - 1;
            if (y >= rh) y = rh - 1;
            pin_at(pixel_to_norm(x, rw), pixel_to_norm(y, rh));
        }
        if (ImGui::IsItemHovered() && ref.w > 0) {
            ImGui::SetTooltip(
                "Add a pinned sample at (x, y) in A's native pixels\n"
                "(range: 0..%d, 0..%d).",
                ref.w - 1, ref.h - 1);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(pinned_.empty());
        if (ImGui::SmallButton("Clear")) {
            clear_pins();
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Remove all pinned samples");
        }
    }
    ImGui::TextDisabled(
        "Tip: hover + P to pin, or Shift+Click on the image.");

    if (samples.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("No images loaded.");
        return;
    }

    // --- Table ----------------------------------------------------------
    constexpr ImGuiTableFlags table_flags =
        ImGuiTableFlags_BordersInnerV |
        ImGuiTableFlags_RowBg |
        ImGuiTableFlags_SizingStretchProp |
        ImGuiTableFlags_ScrollY;

    if (!ImGui::BeginTable("##pixel_inspector_table", 6, table_flags,
                            ImVec2(0.0f, 0.0f))) {
        return;
    }
    ImGui::TableSetupColumn("#",      ImGuiTableColumnFlags_WidthFixed, 22.0f);
    ImGui::TableSetupColumn("Label",  ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("Coord",  ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("Value",  ImGuiTableColumnFlags_WidthStretch, 1.4f);
    ImGui::TableSetupColumn("Delta",  ImGuiTableColumnFlags_WidthStretch, 1.4f);
    ImGui::TableSetupColumn("##act",  ImGuiTableColumnFlags_WidthFixed |
                                       ImGuiTableColumnFlags_NoHeaderLabel, 26.0f);
    ImGui::TableHeadersRow();

    // Helper that renders all sample-vs-image rows for one sample
    // point.  `point_idx` is the row number to print in the # column
    // (-1 for hover); `editable` decides whether the Coord column
    // shows an InputInt2 or plain text.  Returns:
    //   0 = no action, 1 = delete this point.
    auto render_point = [&](PixelSamplePoint& point,
                             int point_idx,
                             bool editable) -> int {
        // Reset clamp marker -- it's only meant to flash for one frame.
        bool was_clamped = point.clamp_marker;
        point.clamp_marker = false;

        // Sample A once so every other row can compute its delta.
        PixelSample a_sample{};
        if (!samples.empty() && samples[0].second &&
            !samples[0].second->mat().empty()) {
            a_sample = sample_image(samples[0].second->mat(), point.u, point.v);
        }

        // Pre-compute integer coords using the reference resolution.
        int ref_x = (ref.w > 0) ? norm_to_pixel(point.u, ref.w) : -1;
        int ref_y = (ref.h > 0) ? norm_to_pixel(point.v, ref.h) : -1;

        int action = 0;  // 0=none, 1=delete

        for (size_t i = 0; i < samples.size(); ++i) {
            const std::string& name = samples[i].first;
            const Image* img = samples[i].second;
            const cv::Mat empty;
            const cv::Mat& mat = img ? img->mat() : empty;

            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();

            // # column: print only on the first row of the group.
            ImGui::TableSetColumnIndex(0);
            if (i == 0) {
                if (point_idx < 0) ImGui::TextUnformatted("H");
                else                ImGui::Text("%d", point_idx);
            }

            // Label
            ImGui::TableSetColumnIndex(1);
            std::string row_label = format_row_label(static_cast<int>(i), name);
            if (was_clamped) {
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                                   "%s", row_label.c_str());
            } else {
                ImGui::TextUnformatted(row_label.c_str());
            }

            // Sample once for both Value and Delta columns.
            PixelSample s = (!mat.empty())
                ? sample_image(mat, point.u, point.v)
                : PixelSample{};

            // Coord (x, y) column.  Editable on the first row of a
            // pinned point (when ref resolution exists).  Other rows
            // print the entry's own native coords.  Hover and pre-A
            // rows are read-only.
            ImGui::TableSetColumnIndex(2);
            if (editable && i == 0 && ref.w > 0 && ref.h > 0) {
                int xy[2] = { ref_x, ref_y };
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::InputInt2("##xy", xy)) {
                    if (xy[0] < 0) xy[0] = 0;
                    if (xy[1] < 0) xy[1] = 0;
                    if (xy[0] >= ref.w) xy[0] = ref.w - 1;
                    if (xy[1] >= ref.h) xy[1] = ref.h - 1;
                    point.u = pixel_to_norm(xy[0], ref.w);
                    point.v = pixel_to_norm(xy[1], ref.h);
                    point.clamp_marker = true;
                }
            } else if (mat.empty()) {
                ImGui::TextUnformatted(kEmDash);
            } else {
                int x = norm_to_pixel(point.u, mat.cols);
                int y = norm_to_pixel(point.v, mat.rows);
                ImGui::Text("(%d, %d)", x, y);
            }

            // Value column
            ImGui::TableSetColumnIndex(3);
            if (mat.empty()) {
                ImGui::TextUnformatted(kEmDash);
            } else {
                char vbuf[96];
                format_pixel(s, vbuf, sizeof(vbuf));
                ImGui::TextUnformatted(vbuf);
                if (!s.valid && ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(
                        "out of range (image is %d x %d)",
                        mat.cols, mat.rows);
                }
            }

            // Delta column (entry - A); A's own row prints zeros.
            ImGui::TableSetColumnIndex(4);
            char dbuf[96];
            format_delta(s, a_sample, dbuf, sizeof(dbuf));
            ImGui::TextUnformatted(dbuf);

            // Action column.  Hover row leaves it blank (the Pin
            // button used to live here, but moving the cursor to it
            // invalidates the hover before the click lands; users
            // pin via Alt+P / Shift+Click instead).  Pinned rows
            // keep the "x" delete button on the first row only.
            ImGui::TableSetColumnIndex(5);
            if (i == 0 && point_idx >= 0) {
                if (ImGui::SmallButton("x")) {
                    action = 1;
                }
            }

            ImGui::PopID();
        }

        return action;
    };

    // --- Hover row group -----------------------------------------------
    {
        ImGui::PushID("hover");
        PixelSamplePoint hover_view = hover_;
        if (!hover_valid_) {
            hover_view.u = -1.0;
            hover_view.v = -1.0;
        }
        // The hover row has no Pin button anymore; the only action a
        // render_point() call could surface here is "delete", which is
        // impossible for the (non-pinned) hover row.  We still ignore
        // the return value defensively in case the contract changes.
        (void)render_point(hover_view, -1, /*editable=*/false);
        ImGui::PopID();
    }

    // --- Pinned rows ----------------------------------------------------
    int delete_idx = -1;
    for (size_t pi = 0; pi < pinned_.size(); ++pi) {
        ImGui::PushID(static_cast<int>(pi) + 1);
        int act = render_point(pinned_[pi],
                               static_cast<int>(pi + 1),
                               /*editable=*/true);
        if (act == 1) delete_idx = static_cast<int>(pi);
        ImGui::PopID();
    }
    if (delete_idx >= 0 &&
        delete_idx < static_cast<int>(pinned_.size())) {
        pinned_.erase(pinned_.begin() + delete_idx);
    }

    ImGui::EndTable();
}

} // namespace idiff
