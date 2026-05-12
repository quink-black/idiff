#include "app/viewport.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>

#include "core/ruler_utils.h"

namespace idiff {

Viewport::Viewport() = default;

ImTextureID Viewport::to_tex_id(SDL_Texture* tex) {
    return (ImTextureID)(uintptr_t)tex;
}

// --- Zoom / pan helpers ---

void Viewport::set_zoom(float z) {
    zoom_ = std::clamp(z, 0.05f, 64.0f);
}

void Viewport::zoom_around(float new_zoom, ImVec2 anchor) {
    new_zoom = std::clamp(new_zoom, 0.05f, 64.0f);
    if (new_zoom == zoom_) return;

    ImVec2 cell_org, cell_sz;
    cell_at(anchor, cell_org, cell_sz);

    float cx = cell_org.x + cell_sz.x * 0.5f + pan_x_;
    float cy = cell_org.y + cell_sz.y * 0.5f + pan_y_;

    float ratio = 1.0f - new_zoom / zoom_;
    pan_x_ += (anchor.x - cx) * ratio;
    pan_y_ += (anchor.y - cy) * ratio;

    zoom_ = new_zoom;
}

void Viewport::zoom_to_rect(ImVec2 rect_min, ImVec2 rect_max) {
    float rw = rect_max.x - rect_min.x;
    float rh = rect_max.y - rect_min.y;
    if (rw < 4.0f || rh < 4.0f || vp_size_.x < 1.0f || vp_size_.y < 1.0f) return;

    ImVec2 sel_center((rect_min.x + rect_max.x) * 0.5f,
                      (rect_min.y + rect_max.y) * 0.5f);
    ImVec2 cell_org, cell_sz;
    cell_at(sel_center, cell_org, cell_sz);

    float sel_cx = sel_center.x;
    float sel_cy = sel_center.y;

    float cell_cx = cell_org.x + cell_sz.x * 0.5f;
    float cell_cy = cell_org.y + cell_sz.y * 0.5f;

    float scale_factor = std::min(cell_sz.x / rw, cell_sz.y / rh);

    float new_zoom = std::clamp(zoom_ * scale_factor, 0.05f, 64.0f);
    float actual_factor = new_zoom / zoom_;

    pan_x_ = -(sel_cx - cell_cx - pan_x_) * actual_factor;
    pan_y_ = -(sel_cy - cell_cy - pan_y_) * actual_factor;
    zoom_ = new_zoom;
}

void Viewport::fit_to_content() {
    zoom_ = 1.0f;
    pan_x_ = 0.0f;
    pan_y_ = 0.0f;
}

void Viewport::zoom_to_actual() {
    if (content_w_ <= 0 || content_h_ <= 0 || vp_size_.x <= 0 || vp_size_.y <= 0) {
        zoom_ = 1.0f;
        pan_x_ = 0.0f;
        pan_y_ = 0.0f;
        return;
    }

    float area_w = vp_size_.x / split_cols_;
    float area_h = vp_size_.y / split_rows_;
    float fit_scale = std::min(area_w / content_w_, area_h / content_h_);
    float new_zoom = 1.0f / fit_scale;
    pan_x_ = 0.0f;
    pan_y_ = 0.0f;
    zoom_ = std::clamp(new_zoom, 0.05f, 64.0f);
}

// --- Selection rectangle ---

void Viewport::begin_selection(ImVec2 start) {
    selecting_ = true;
    sel_start_ = start;
    sel_min_ = start;
    sel_max_ = start;
}

void Viewport::update_selection(ImVec2 current) {
    sel_min_ = ImVec2(std::min(sel_start_.x, current.x), std::min(sel_start_.y, current.y));
    sel_max_ = ImVec2(std::max(sel_start_.x, current.x), std::max(sel_start_.y, current.y));
}

void Viewport::end_selection() {
    selecting_ = false;
    zoom_to_rect(sel_min_, sel_max_);
}

void Viewport::cancel_selection() {
    selecting_ = false;
}

void Viewport::draw_selection_rect() {
    if (!selecting_) return;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRect(sel_min_, sel_max_, IM_COL32(255, 200, 50, 220), 0.0f, 0, 2.0f);
    dl->AddRectFilled(sel_min_, sel_max_, IM_COL32(255, 200, 50, 30));
}

// --- Measurement ---

bool Viewport::resolve_cell_at(ImVec2 screen_pt,
                               int& cell_index,
                               int& tex_w, int& tex_h,
                               ImVec2& cell_origin, ImVec2& cell_size) const {
    if (screen_pt.x < vp_origin_.x || screen_pt.x >= vp_origin_.x + vp_size_.x ||
        screen_pt.y < vp_origin_.y || screen_pt.y >= vp_origin_.y + vp_size_.y) {
        return false;
    }

    float cell_w = vp_size_.x / split_cols_;
    float cell_h = vp_size_.y / split_rows_;
    int col = static_cast<int>((screen_pt.x - vp_origin_.x) / cell_w);
    int row = static_cast<int>((screen_pt.y - vp_origin_.y) / cell_h);
    col = std::clamp(col, 0, split_cols_ - 1);
    row = std::clamp(row, 0, split_rows_ - 1);
    int grid_idx = row * split_cols_ + col;

    // Map the on-screen grid cell to a logical slot index into
    // cell_layouts_.  In Split/Difference the mapping is identity.  In
    // Overlay the single grid cell hosts two slots (A and B); pick the
    // one whose half of the A/B slider the cursor is over so the
    // measurement is recorded against the correct source image.
    int slot = grid_idx;
    if (mode_ == ComparisonMode::Overlay && cell_layouts_.size() >= 2) {
        float slider_screen_x = vp_origin_.x + vp_size_.x * slider_pos_;
        slot = (screen_pt.x < slider_screen_x) ? 0 : 1;
    }

    if (slot < 0 || slot >= static_cast<int>(cell_layouts_.size())) return false;
    const auto& cl = cell_layouts_[slot];
    if (cl.tex_w <= 0 || cl.tex_h <= 0) return false;

    cell_index = slot;
    tex_w = cl.tex_w;
    tex_h = cl.tex_h;
    cell_origin = ImVec2(vp_origin_.x + col * cell_w,
                         vp_origin_.y + row * cell_h);
    cell_size = ImVec2(cell_w, cell_h);
    return true;
}

void Viewport::screen_to_image_px(ImVec2 screen_pt,
                                   int cell_index,
                                   int tex_w, int tex_h,
                                   ImVec2 cell_origin, ImVec2 cell_size,
                                   float& out_px, float& out_py) const {
    // Screen layout is driven by the composite canvas size (which equals
    // tex_w/h outside Overlay).  The measurement coordinate system is
    // the source image's native pixels, so the per-pixel screen scale is
    // (composite_screen_size / composite_w) * (composite_w / tex_w),
    // which simplifies to fit_scale_composite * (composite_w / tex_w).
    int comp_w = tex_w;
    int comp_h = tex_h;
    if (cell_index >= 0 && cell_index < static_cast<int>(cell_layouts_.size())) {
        const auto& cl = cell_layouts_[cell_index];
        if (cl.composite_w > 0 && cl.composite_h > 0) {
            comp_w = cl.composite_w;
            comp_h = cl.composite_h;
        }
    }

    float fit_scale = std::min(cell_size.x / comp_w, cell_size.y / comp_h);
    float zoom = fit_scale * zoom_;
    if (zoom <= 0.0f || tex_w <= 0 || tex_h <= 0) {
        out_px = 0.0f; out_py = 0.0f; return;
    }

    float disp_w = comp_w * zoom;
    float disp_h = comp_h * zoom;
    float img_x = cell_origin.x + (cell_size.x - disp_w) * 0.5f + pan_x_;
    float img_y = cell_origin.y + (cell_size.y - disp_h) * 0.5f + pan_y_;

    // Convert from screen to composite-space pixels, then rescale into
    // the source image's own pixel grid.
    float comp_px = (screen_pt.x - img_x) / zoom;
    float comp_py = (screen_pt.y - img_y) / zoom;
    out_px = comp_px * static_cast<float>(tex_w) / static_cast<float>(comp_w);
    out_py = comp_py * static_cast<float>(tex_h) / static_cast<float>(comp_h);
}

void Viewport::image_px_to_screen(int cell_index,
                                   int tex_w, int tex_h,
                                   float px, float py,
                                   ImVec2& out_screen) const {
    // Screen projection mirrors screen_to_image_px: lay out the cell on
    // the composite canvas, then map the source-pixel coordinate into
    // composite space before applying the screen scale.
    int cols = std::max(1, split_cols_);
    int rows = std::max(1, split_rows_);

    int comp_w = tex_w;
    int comp_h = tex_h;
    int grid_idx = std::clamp(cell_index, 0, cols * rows - 1);
    if (cell_index >= 0 && cell_index < static_cast<int>(cell_layouts_.size())) {
        const auto& cl = cell_layouts_[cell_index];
        if (cl.composite_w > 0 && cl.composite_h > 0) {
            comp_w = cl.composite_w;
            comp_h = cl.composite_h;
        }
        // grid_cell remembers which on-screen cell hosts this slot; in
        // Overlay multiple slots collapse onto grid cell 0.
        grid_idx = std::clamp(cl.grid_cell, 0, cols * rows - 1);
    }

    float cell_w = vp_size_.x / cols;
    float cell_h = vp_size_.y / rows;
    int col = grid_idx % cols;
    int row = grid_idx / cols;
    float cell_x = vp_origin_.x + col * cell_w;
    float cell_y = vp_origin_.y + row * cell_h;

    if (comp_w <= 0 || comp_h <= 0 || tex_w <= 0 || tex_h <= 0) {
        out_screen = ImVec2(cell_x, cell_y);
        return;
    }

    float fit_scale = std::min(cell_w / comp_w, cell_h / comp_h);
    float zoom = fit_scale * zoom_;
    float disp_w = comp_w * zoom;
    float disp_h = comp_h * zoom;
    float img_x = cell_x + (cell_w - disp_w) * 0.5f + pan_x_;
    float img_y = cell_y + (cell_h - disp_h) * 0.5f + pan_y_;

    // Source pixel -> composite pixel -> screen.
    float comp_px = px * static_cast<float>(comp_w) / static_cast<float>(tex_w);
    float comp_py = py * static_cast<float>(comp_h) / static_cast<float>(tex_h);
    out_screen = ImVec2(img_x + comp_px * zoom, img_y + comp_py * zoom);
}

void Viewport::begin_measurement(ImVec2 screen_pt) {
    int cell_idx, tw, th;
    ImVec2 co, cs;
    if (!resolve_cell_at(screen_pt, cell_idx, tw, th, co, cs)) {
        measuring_ = false;
        return;
    }
    float px, py;
    screen_to_image_px(screen_pt, cell_idx, tw, th, co, cs, px, py);

    measuring_ = true;
    meas_drag_cell_ = cell_idx;
    meas_drag_tex_w_ = tw;
    meas_drag_tex_h_ = th;
    meas_drag_x0_ = meas_drag_x1_ = px;
    meas_drag_y0_ = meas_drag_y1_ = py;
}

void Viewport::update_measurement(ImVec2 screen_pt) {
    if (!measuring_ || meas_drag_cell_ < 0) return;
    // The drag is anchored on the slot resolved at mouse-down.  Recompute
    // its on-screen grid cell rect from the last-rendered grid so pan/
    // zoom between frames does not corrupt the mapping.  In Overlay,
    // multiple slots map onto the same grid cell (cell 0).
    int cols = std::max(1, split_cols_);
    int rows = std::max(1, split_rows_);
    int grid_idx = meas_drag_cell_;
    if (meas_drag_cell_ >= 0 &&
        meas_drag_cell_ < static_cast<int>(cell_layouts_.size())) {
        grid_idx = cell_layouts_[meas_drag_cell_].grid_cell;
    }
    grid_idx = std::clamp(grid_idx, 0, cols * rows - 1);
    float cell_w = vp_size_.x / cols;
    float cell_h = vp_size_.y / rows;
    int col = grid_idx % cols;
    int row = grid_idx / cols;
    ImVec2 co(vp_origin_.x + col * cell_w, vp_origin_.y + row * cell_h);
    ImVec2 cs(cell_w, cell_h);

    float px, py;
    screen_to_image_px(screen_pt, meas_drag_cell_,
                       meas_drag_tex_w_, meas_drag_tex_h_, co, cs,
                       px, py);
    meas_drag_x1_ = px;
    meas_drag_y1_ = py;
}

const Measurement* Viewport::end_measurement() {
    if (!measuring_) return nullptr;
    measuring_ = false;

    float w = std::abs(meas_drag_x1_ - meas_drag_x0_);
    float h = std::abs(meas_drag_y1_ - meas_drag_y0_);
    // Discard near-zero boxes (treated as a stray click).  One-pixel
    // resolution is the smallest meaningful unit for the source image.
    if (w < 1.0f || h < 1.0f) return nullptr;

    Measurement m;
    m.id = next_measurement_id_++;
    m.source_cell_index = meas_drag_cell_;
    m.src_tex_w = meas_drag_tex_w_;
    m.src_tex_h = meas_drag_tex_h_;
    m.x0 = std::min(meas_drag_x0_, meas_drag_x1_);
    m.y0 = std::min(meas_drag_y0_, meas_drag_y1_);
    m.x1 = std::max(meas_drag_x0_, meas_drag_x1_);
    m.y1 = std::max(meas_drag_y0_, meas_drag_y1_);
    measurements_.push_back(m);
    return &measurements_.back();
}

void Viewport::cancel_measurement() {
    measuring_ = false;
}

void Viewport::remove_measurement(int id) {
    measurements_.erase(
        std::remove_if(measurements_.begin(), measurements_.end(),
                       [id](const Measurement& m) { return m.id == id; }),
        measurements_.end());
}

void Viewport::clear_measurements() {
    measurements_.clear();
}

void Viewport::add_measurement(const Measurement& m) {
    measurements_.push_back(m);
}

void Viewport::draw_measurements() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 stroke_saved = IM_COL32(0x33, 0xE6, 0xFF, 230);
    const ImU32 fill_saved   = IM_COL32(0x33, 0xE6, 0xFF, 25);
    const ImU32 stroke_live  = IM_COL32(0xFF, 0xC8, 0x32, 220);
    const ImU32 fill_live    = IM_COL32(0xFF, 0xC8, 0x32, 30);
    const ImU32 label_bg     = IM_COL32(0, 0, 0, 180);
    const ImU32 label_fg     = IM_COL32(0xFF, 0xFF, 0xFF, 235);
    const ImU32 x_bg         = IM_COL32(0, 0, 0, 200);
    const ImU32 x_bg_hot     = IM_COL32(0xFF, 0x55, 0x55, 230);
    const ImU32 x_fg         = IM_COL32(0xFF, 0xFF, 0xFF, 235);

    hover_measurement_id_ = -1;
    hover_close_hot_ = false;

    // Live drag preview (drawn first so it sits under saved rects visually
    // if they overlap; the live box is only visible during the drag).
    if (measuring_ && meas_drag_cell_ >= 0 && meas_drag_tex_w_ > 0) {
        ImVec2 p0, p1;
        image_px_to_screen(meas_drag_cell_, meas_drag_tex_w_, meas_drag_tex_h_,
                           std::min(meas_drag_x0_, meas_drag_x1_),
                           std::min(meas_drag_y0_, meas_drag_y1_), p0);
        image_px_to_screen(meas_drag_cell_, meas_drag_tex_w_, meas_drag_tex_h_,
                           std::max(meas_drag_x0_, meas_drag_x1_),
                           std::max(meas_drag_y0_, meas_drag_y1_), p1);
        dl->AddRectFilled(p0, p1, fill_live);
        dl->AddRect(p0, p1, stroke_live, 0.0f, 0, 2.0f);

        int w_px = static_cast<int>(std::round(std::abs(meas_drag_x1_ - meas_drag_x0_)));
        int h_px = static_cast<int>(std::round(std::abs(meas_drag_y1_ - meas_drag_y0_)));
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%d x %d px", w_px, h_px);
        ImVec2 ts = ImGui::CalcTextSize(buf);
        ImVec2 lp(p0.x, p0.y - ts.y - 6.0f);
        if (lp.y < vp_origin_.y + 2.0f) lp.y = p0.y + 4.0f;
        dl->AddRectFilled(ImVec2(lp.x - 4, lp.y - 2),
                          ImVec2(lp.x + ts.x + 4, lp.y + ts.y + 2),
                          label_bg, 3.0f);
        dl->AddText(lp, label_fg, buf);
    }

    // Saved measurements.
    ImVec2 mouse = ImGui::GetIO().MousePos;
    for (const auto& m : measurements_) {
        ImVec2 p0, p1;
        image_px_to_screen(m.source_cell_index, m.src_tex_w, m.src_tex_h,
                           m.x0, m.y0, p0);
        image_px_to_screen(m.source_cell_index, m.src_tex_w, m.src_tex_h,
                           m.x1, m.y1, p1);

        dl->AddRectFilled(p0, p1, fill_saved);
        dl->AddRect(p0, p1, stroke_saved, 0.0f, 0, 2.0f);

        int w_px = static_cast<int>(std::round(m.x1 - m.x0));
        int h_px = static_cast<int>(std::round(m.y1 - m.y0));
        char buf[80];
        std::snprintf(buf, sizeof(buf), "#%d  %d x %d px", m.id, w_px, h_px);
        ImVec2 ts = ImGui::CalcTextSize(buf);
        ImVec2 lp(p0.x, p0.y - ts.y - 6.0f);
        if (lp.y < vp_origin_.y + 2.0f) lp.y = p0.y + 4.0f;
        dl->AddRectFilled(ImVec2(lp.x - 4, lp.y - 2),
                          ImVec2(lp.x + ts.x + 4, lp.y + ts.y + 2),
                          label_bg, 3.0f);
        dl->AddText(lp, label_fg, buf);

        // Hover detection + delete button.  We check hover against the
        // rectangle's bounding box (order-insensitive for p0/p1).
        float bx0 = std::min(p0.x, p1.x);
        float by0 = std::min(p0.y, p1.y);
        float bx1 = std::max(p0.x, p1.x);
        float by1 = std::max(p0.y, p1.y);
        bool hovered = mouse.x >= bx0 && mouse.x <= bx1 &&
                       mouse.y >= by0 && mouse.y <= by1;
        if (hovered) {
            hover_measurement_id_ = m.id;

            // x button at the top-right corner of the rect (inside the
            // viewport clip so it never leaks into the menu bar).
            const float sz = 16.0f;
            ImVec2 xr_min(std::min(bx1, vp_origin_.x + vp_size_.x) - sz - 2.0f,
                          std::max(by0, vp_origin_.y) + 2.0f);
            ImVec2 xr_max(xr_min.x + sz, xr_min.y + sz);

            bool x_hot = mouse.x >= xr_min.x && mouse.x <= xr_max.x &&
                         mouse.y >= xr_min.y && mouse.y <= xr_max.y;
            if (x_hot) hover_close_hot_ = true;
            dl->AddRectFilled(xr_min, xr_max, x_hot ? x_bg_hot : x_bg, 3.0f);
            // Draw the X glyph as two strokes so it stays readable at any
            // font.  Padding keeps the strokes clear of the box edges.
            float pad = 4.0f;
            dl->AddLine(ImVec2(xr_min.x + pad, xr_min.y + pad),
                        ImVec2(xr_max.x - pad, xr_max.y - pad),
                        x_fg, 1.6f);
            dl->AddLine(ImVec2(xr_max.x - pad, xr_min.y + pad),
                        ImVec2(xr_min.x + pad, xr_max.y - pad),
                        x_fg, 1.6f);

            if (x_hot && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                pending_delete_id_ = m.id;
            }
        }
    }

    // Apply pending delete from the x button.  The measurement is removed
    // from the viewport display immediately; App reads
    // consume_pending_delete() after render to also remove it from the
    // owning ImageEntry.
    if (pending_delete_id_ >= 0) {
        int id = pending_delete_id_;
        pending_delete_id_ = -1;
        remove_measurement(id);
        last_deleted_id_ = id;
    }
}

// --- Grid layout computation ---

void Viewport::compute_grid(int n, GridLayout layout, int user_cols,
                             int& cols, int& rows) {
    if (n <= 0) { cols = 1; rows = 1; return; }

    switch (layout) {
        case GridLayout::SingleRow:
            cols = n;
            rows = 1;
            break;
        case GridLayout::SingleCol:
            cols = 1;
            rows = n;
            break;
        case GridLayout::RowsCols:
            cols = std::max(1, user_cols);
            rows = (n + cols - 1) / cols;
            break;
        case GridLayout::Auto:
        default:
            if (n == 1) { cols = 1; rows = 1; }
            else if (n == 2) { cols = 2; rows = 1; }
            else if (n <= 4) { cols = 2; rows = 2; }
            else if (n <= 6) { cols = 3; rows = 2; }
            else { cols = 3; rows = (n + cols - 1) / cols; }
            break;
    }
}

// --- Drawing helpers ---

void Viewport::draw_image_label(const char* label,
                                 ImVec2 img_pos, ImVec2 img_size,
                                 ImVec2 cell_pos, ImVec2 cell_size,
                                 bool ruler_visible) {
    // Labels used to sit in the top-left of the image with a solid
    // background box, which worked for "one image fills the cell" cases
    // but aggressively covered a corner of the actual image in split /
    // overlay / diff modes.  The new strategy:
    //   1. Prefer drawing the label *above* the image rect, inside the
    //      owning cell.  That way pixels are never occluded.
    //   2. If there isn't enough vertical room above the image (e.g.
    //      the image already hugs the cell top, which is common when
    //      zoomed out so the image fits exactly), fall back to placing
    //      the label inside the image but with a low-alpha background
    //      so it stays readable without fully hiding pixels.
    // When the ruler is visible, the top 18 px of the cell are occupied
    // by the ruler strip, so the label must avoid that region.
    const float font_scale = 0.88f;
    const float pad_x = 4.0f;
    const float pad_y = 2.0f;
    constexpr float ruler_thickness = 18.0f;

    ImFont* font = ImGui::GetFont();
    float font_size = ImGui::GetFontSize() * font_scale;
    ImVec2 text_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, label);

    float box_w = text_size.x + pad_x * 2;
    float box_h = text_size.y + pad_y * 2;

    // The effective top of the cell, accounting for the ruler strip.
    float cell_effective_top = ruler_visible
                                   ? cell_pos.y + ruler_thickness
                                   : cell_pos.y;

    // Candidate 1: just above the image rect, anchored to image left.
    float above_y = img_pos.y - box_h - 2.0f;
    bool fits_above = above_y >= cell_effective_top;

    ImVec2 rect_min;
    ImU32 bg_color;
    if (fits_above) {
        float x = std::clamp(img_pos.x,
                             cell_pos.x,
                             cell_pos.x + cell_size.x - box_w);
        rect_min = ImVec2(x, above_y);
        // Solid, high-contrast badge when we have dedicated room above
        // the image: the label is not on top of any pixels so we can
        // afford full opacity.
        bg_color = IM_COL32(0, 0, 0, 200);
    } else {
        // Fallback: overlay on the image, but below the ruler strip and
        // subtle enough that the underlying pixels still show through.
        float x = std::clamp(img_pos.x + 2.0f,
                             cell_pos.x,
                             cell_pos.x + cell_size.x - box_w);
        float y = std::max(img_pos.y + 2.0f, cell_effective_top + 2.0f);
        rect_min = ImVec2(x, y);
        bg_color = IM_COL32(0, 0, 0, 110);
    }

    ImVec2 rect_max(rect_min.x + box_w, rect_min.y + box_h);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(rect_min, rect_max, bg_color, 3.0f);
    dl->AddText(font, font_size,
                ImVec2(rect_min.x + pad_x, rect_min.y + pad_y),
                IM_COL32(255, 255, 255, 235), label);
}

void Viewport::compute_image_rect(int img_w, int img_h,
                                   ImVec2& out_pos, ImVec2& out_size) const {
    float fit_scale = std::min(vp_size_.x / img_w, vp_size_.y / img_h);
    float scale = fit_scale * zoom_;

    float disp_w = img_w * scale;
    float disp_h = img_h * scale;

    float x = vp_origin_.x + (vp_size_.x - disp_w) * 0.5f + pan_x_;
    float y = vp_origin_.y + (vp_size_.y - disp_h) * 0.5f + pan_y_;

    out_pos = ImVec2(x, y);
    out_size = ImVec2(disp_w, disp_h);
}

// --- Ruler and Grid ---

void Viewport::draw_ruler(ImVec2 img_pos, ImVec2 img_size,
                           int img_w, int img_h, float scale,
                           ImVec2 cell_origin, ImVec2 cell_size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    constexpr float ruler_thickness = 18.0f;
    constexpr float min_tick_spacing = 50.0f;
    ImU32 bg_color = IM_COL32(40, 40, 40, 220);
    ImU32 tick_color = IM_COL32(180, 180, 180, 220);
    ImU32 label_color = IM_COL32(200, 200, 200, 255);
    ImU32 border_color = IM_COL32(80, 80, 80, 255);

    int interval = compute_nice_interval(scale, min_tick_spacing);

    float cell_left = cell_origin.x;
    float cell_top = cell_origin.y;
    float cell_right = cell_origin.x + cell_size.x;
    float cell_bottom = cell_origin.y + cell_size.y;

    // Horizontal ruler strip: always at the top of the cell
    float h_ruler_y = cell_top;
    float h_ruler_bottom = cell_top + ruler_thickness;
    float h_border_y = h_ruler_bottom;

    // Vertical ruler strip: always at the left of the cell
    float v_ruler_x = cell_left;
    float v_ruler_right = cell_left + ruler_thickness;
    float v_border_x = v_ruler_right;

    // --- Horizontal ruler ---
    {
        dl->AddRectFilled(ImVec2(cell_left, h_ruler_y),
                          ImVec2(cell_right, h_ruler_bottom),
                          bg_color);
        dl->AddLine(ImVec2(cell_left, h_border_y),
                    ImVec2(cell_right, h_border_y),
                    border_color);

        // Only iterate ticks within the visible pixel range
        int px_start = std::max(0, static_cast<int>((cell_left - img_pos.x) / scale));
        int px_end = std::min(img_w, static_cast<int>((cell_right - img_pos.x) / scale) + 1);
        px_start = (px_start / interval) * interval;

        for (int px = px_start; px <= px_end; px += interval) {
            float sx = img_pos.x + px * scale;
            if (sx < cell_left || sx > cell_right) continue;

            dl->AddLine(ImVec2(sx, h_border_y - 8),
                        ImVec2(sx, h_border_y),
                        tick_color);

            char buf[32];
            std::snprintf(buf, sizeof(buf), "%d", px);
            ImVec2 text_size = ImGui::CalcTextSize(buf);
            float label_x = std::clamp(sx + 2.0f, cell_left + 1.0f, cell_right - text_size.x - 1.0f);
            dl->AddText(ImVec2(label_x, h_ruler_y + 1), label_color, buf);
        }

        int minor = std::max(1, interval / 5);
        int minor_start = (px_start / minor) * minor;
        for (int px = minor_start; px <= px_end; px += minor) {
            if (px % interval == 0) continue;
            float sx = img_pos.x + px * scale;
            if (sx < cell_left || sx > cell_right) continue;
            dl->AddLine(ImVec2(sx, h_border_y - 4),
                        ImVec2(sx, h_border_y),
                        IM_COL32(120, 120, 120, 180));
        }
    }

    // --- Vertical ruler ---
    {
        dl->AddRectFilled(ImVec2(v_ruler_x, h_ruler_bottom),
                          ImVec2(v_ruler_right, cell_bottom),
                          bg_color);
        dl->AddLine(ImVec2(v_border_x, h_ruler_bottom),
                    ImVec2(v_border_x, cell_bottom),
                    border_color);

        // Only iterate ticks within the visible pixel range
        int py_start = std::max(0, static_cast<int>((h_ruler_bottom - img_pos.y) / scale));
        int py_end = std::min(img_h, static_cast<int>((cell_bottom - img_pos.y) / scale) + 1);
        py_start = (py_start / interval) * interval;

        for (int py = py_start; py <= py_end; py += interval) {
            float sy = img_pos.y + py * scale;
            if (sy < h_ruler_bottom || sy > cell_bottom) continue;

            dl->AddLine(ImVec2(v_border_x - 8, sy),
                        ImVec2(v_border_x, sy),
                        tick_color);

            char buf[32];
            std::snprintf(buf, sizeof(buf), "%d", py);
            ImVec2 text_size = ImGui::CalcTextSize(buf);
            float label_y = std::clamp(sy + 1.0f, h_ruler_bottom + 1.0f, cell_bottom - text_size.y - 1.0f);
            dl->AddText(ImVec2(v_ruler_x + 2, label_y), label_color, buf);
        }

        int minor = std::max(1, interval / 5);
        int minor_start = (py_start / minor) * minor;
        for (int py = minor_start; py <= py_end; py += minor) {
            if (py % interval == 0) continue;
            float sy = img_pos.y + py * scale;
            if (sy < h_ruler_bottom || sy > cell_bottom) continue;
            dl->AddLine(ImVec2(v_border_x - 4, sy),
                        ImVec2(v_border_x, sy),
                        IM_COL32(120, 120, 120, 180));
        }
    }

    // Corner square (intersection of H and V rulers)
    dl->AddRectFilled(ImVec2(cell_left, cell_top),
                      ImVec2(v_border_x, h_border_y),
                      bg_color);
}

void Viewport::draw_grid(ImVec2 img_pos, ImVec2 img_size,
                          int img_w, int img_h, float scale) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    constexpr float min_line_spacing = 40.0f;
    ImU32 grid_color = IM_COL32(255, 255, 255, 30);

    int interval = compute_nice_interval(scale, min_line_spacing);

    // Vertical lines
    for (int px = interval; px < img_w; px += interval) {
        float sx = img_pos.x + px * scale;
        if (sx < img_pos.x || sx > img_pos.x + img_size.x) continue;
        dl->AddLine(ImVec2(sx, img_pos.y),
                    ImVec2(sx, img_pos.y + img_size.y),
                    grid_color);
    }

    // Horizontal lines
    for (int py = interval; py < img_h; py += interval) {
        float sy = img_pos.y + py * scale;
        if (sy < img_pos.y || sy > img_pos.y + img_size.y) continue;
        dl->AddLine(ImVec2(img_pos.x, sy),
                    ImVec2(img_pos.x + img_size.x, sy),
                    grid_color);
    }
}

// --- Render ---

void Viewport::render(const std::vector<SDL_Texture*>& tex_ptrs,
                      const std::vector<int>& tex_ws,
                      const std::vector<int>& tex_hs,
                      const std::vector<const char*>& labels,
                      const std::vector<SDL_Texture*>& diff_tex_ptrs,
                      const std::vector<int>& diff_tex_ws,
                      const std::vector<int>& diff_tex_hs,
                      const std::vector<const char*>& diff_labels) {
    ImVec2 avail = ImGui::GetContentRegionAvail();
    vp_origin_ = ImGui::GetCursorScreenPos();
    vp_size_ = avail;

    hover_valid_ = false;
    hover_cell_idx_ = -1;
    hover_px_x_ = 0;
    hover_px_y_ = 0;

    if (avail.x < 10 || avail.y < 10) return;

    content_w_ = 0;
    content_h_ = 0;
    for (size_t i = 0; i < tex_ws.size(); i++) {
        content_w_ = std::max(content_w_, tex_ws[i]);
        content_h_ = std::max(content_h_, tex_hs[i]);
    }
    for (size_t i = 0; i < diff_tex_ws.size(); i++) {
        content_w_ = std::max(content_w_, diff_tex_ws[i]);
        content_h_ = std::max(content_h_, diff_tex_hs[i]);
    }

    if (tex_ptrs.empty() && diff_tex_ptrs.empty()) {
        ImVec2 center(avail.x * 0.5f, avail.y * 0.5f);
        ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + center.x - 100,
                                    ImGui::GetCursorPosY() + center.y - 30));
        ImGui::TextDisabled("No images selected");
        ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + center.x - 140,
                                    ImGui::GetCursorPosY() + 8));
        ImGui::TextDisabled("Check images in the Images panel to compare");
        return;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(vp_origin_, ImVec2(vp_origin_.x + vp_size_.x,
                                         vp_origin_.y + vp_size_.y), true);

    if (mode_ != ComparisonMode::Split) {
        split_cols_ = 1;
        split_rows_ = 1;
    }
    slider_dragging_ = false;

    // Rebuild per-slot tex dimensions.  Downstream measurement code
    // resolves slots through `cell_layouts_`, so the table must mirror
    // the grid drawn below for the current mode.  In Split/Difference,
    // each slot is its own grid cell and the composite size equals the
    // source size.  In Overlay, both slots share grid cell 0 but each
    // keeps its native source size; the composite (max of A and B) is
    // the canvas the renderer stretches both sources onto.
    cell_layouts_.clear();
    switch (mode_) {
        case ComparisonMode::Split:
            cell_layouts_.reserve(tex_ptrs.size());
            for (size_t i = 0; i < tex_ptrs.size(); ++i) {
                CellLayout cl;
                if (tex_ptrs[i] && tex_ws[i] > 0 && tex_hs[i] > 0) {
                    cl.tex_w = tex_ws[i];
                    cl.tex_h = tex_hs[i];
                    cl.composite_w = tex_ws[i];
                    cl.composite_h = tex_hs[i];
                }
                cl.grid_cell = static_cast<int>(i);
                cell_layouts_.push_back(cl);
            }
            break;
        case ComparisonMode::Overlay: {
            // Overlay shares one grid cell between two slots.  Push one
            // CellLayout per slot so resolve_cell_at can return the slot
            // index based on which side of the A/B slider was clicked,
            // and the per-source tex_w/h drives the reported region size.
            int comp_w = 0, comp_h = 0;
            if (!tex_ptrs.empty() && tex_ptrs[0]) {
                comp_w = tex_ws[0];
                comp_h = tex_hs[0];
            }
            if (tex_ptrs.size() >= 2 && tex_ptrs[1]) {
                comp_w = std::max(comp_w, tex_ws[1]);
                comp_h = std::max(comp_h, tex_hs[1]);
            }
            for (size_t i = 0; i < tex_ptrs.size() && i < 2; ++i) {
                CellLayout cl;
                if (tex_ptrs[i] && tex_ws[i] > 0 && tex_hs[i] > 0) {
                    cl.tex_w = tex_ws[i];
                    cl.tex_h = tex_hs[i];
                    cl.composite_w = comp_w;
                    cl.composite_h = comp_h;
                }
                cl.grid_cell = 0;
                cell_layouts_.push_back(cl);
            }
            break;
        }
        case ComparisonMode::Difference:
            cell_layouts_.reserve(diff_tex_ptrs.size());
            for (size_t i = 0; i < diff_tex_ptrs.size(); ++i) {
                CellLayout cl;
                if (diff_tex_ptrs[i] && diff_tex_ws[i] > 0 && diff_tex_hs[i] > 0) {
                    cl.tex_w = diff_tex_ws[i];
                    cl.tex_h = diff_tex_hs[i];
                    cl.composite_w = diff_tex_ws[i];
                    cl.composite_h = diff_tex_hs[i];
                }
                cl.grid_cell = static_cast<int>(i);
                cell_layouts_.push_back(cl);
            }
            break;
    }

    switch (mode_) {
        case ComparisonMode::Split:
            render_split(tex_ptrs, tex_ws, tex_hs, labels);
            break;
        case ComparisonMode::Overlay:
            render_overlay(tex_ptrs, tex_ws, tex_hs, labels);
            break;
        case ComparisonMode::Difference:
            render_difference(diff_tex_ptrs, diff_tex_ws, diff_tex_hs, diff_labels);
            break;
    }

    // --- Compute hover pixel info ---
    {
        ImVec2 mp = ImGui::GetIO().MousePos;
        bool inside_vp = mp.x >= vp_origin_.x && mp.x < vp_origin_.x + vp_size_.x &&
                         mp.y >= vp_origin_.y && mp.y < vp_origin_.y + vp_size_.y;
        if (inside_vp) {
            if (mode_ == ComparisonMode::Split && !tex_ptrs.empty()) {
                int n = static_cast<int>(tex_ptrs.size());
                float cell_w = vp_size_.x / split_cols_;
                float cell_h = vp_size_.y / split_rows_;
                int col = static_cast<int>((mp.x - vp_origin_.x) / cell_w);
                int row = static_cast<int>((mp.y - vp_origin_.y) / cell_h);
                col = std::clamp(col, 0, split_cols_ - 1);
                row = std::clamp(row, 0, split_rows_ - 1);
                int idx = row * split_cols_ + col;
                if (idx < n && tex_ptrs[idx] && tex_ws[idx] > 0 && tex_hs[idx] > 0) {
                    float cell_x = vp_origin_.x + col * cell_w;
                    float cell_y = vp_origin_.y + row * cell_h;
                    float fit_scale = std::min(cell_w / tex_ws[idx],
                                               cell_h / tex_hs[idx]);
                    float scale = fit_scale * zoom_;
                    float disp_w = tex_ws[idx] * scale;
                    float disp_h = tex_hs[idx] * scale;
                    float img_x = cell_x + (cell_w - disp_w) * 0.5f + pan_x_;
                    float img_y = cell_y + (cell_h - disp_h) * 0.5f + pan_y_;
                    if (scale > 0.0f) {
                        int px = static_cast<int>((mp.x - img_x) / scale);
                        int py = static_cast<int>((mp.y - img_y) / scale);
                        if (px >= 0 && px < tex_ws[idx] &&
                            py >= 0 && py < tex_hs[idx]) {
                            hover_valid_ = true;
                            hover_cell_idx_ = idx;
                            hover_px_x_ = px;
                            hover_px_y_ = py;
                        }
                    }
                }
            } else if (mode_ == ComparisonMode::Overlay && !tex_ptrs.empty()) {
                int img_w = tex_ws[0];
                int img_h = tex_hs[0];
                int cell = 0;
                if (tex_ptrs.size() >= 2 && tex_ptrs[1]) {
                    img_w = std::max(tex_ws[0], tex_ws[1]);
                    img_h = std::max(tex_hs[0], tex_hs[1]);
                    float slider_screen_x = vp_origin_.x + vp_size_.x * slider_pos_;
                    cell = (mp.x < slider_screen_x) ? 0 : 1;
                }
                if (img_w > 0 && img_h > 0) {
                    ImVec2 pos, size;
                    compute_image_rect(img_w, img_h, pos, size);
                    if (size.x > 0.0f && size.y > 0.0f) {
                        float scale = size.x / img_w;
                        int px = static_cast<int>((mp.x - pos.x) / scale);
                        int py = static_cast<int>((mp.y - pos.y) / scale);
                        int real_w = (cell == 1 && tex_ptrs.size() >= 2)
                                         ? tex_ws[1] : tex_ws[0];
                        int real_h = (cell == 1 && tex_ptrs.size() >= 2)
                                         ? tex_hs[1] : tex_hs[0];
                        if (px >= 0 && px < real_w && py >= 0 && py < real_h) {
                            hover_valid_ = true;
                            hover_cell_idx_ = cell;
                            hover_px_x_ = px;
                            hover_px_y_ = py;
                        }
                    }
                }
            } else if (mode_ == ComparisonMode::Difference && !diff_tex_ptrs.empty()) {
                int n = static_cast<int>(diff_tex_ptrs.size());
                float cell_w = vp_size_.x / split_cols_;
                float cell_h = vp_size_.y / split_rows_;
                int col = static_cast<int>((mp.x - vp_origin_.x) / cell_w);
                int row = static_cast<int>((mp.y - vp_origin_.y) / cell_h);
                col = std::clamp(col, 0, split_cols_ - 1);
                row = std::clamp(row, 0, split_rows_ - 1);
                int idx = row * split_cols_ + col;
                if (idx < n && diff_tex_ptrs[idx] &&
                    diff_tex_ws[idx] > 0 && diff_tex_hs[idx] > 0) {
                    float cell_x = vp_origin_.x + col * cell_w;
                    float cell_y = vp_origin_.y + row * cell_h;
                    float fit_scale = std::min(cell_w / diff_tex_ws[idx],
                                               cell_h / diff_tex_hs[idx]);
                    float scale = fit_scale * zoom_;
                    float disp_w = diff_tex_ws[idx] * scale;
                    float disp_h = diff_tex_hs[idx] * scale;
                    float img_x = cell_x + (cell_w - disp_w) * 0.5f + pan_x_;
                    float img_y = cell_y + (cell_h - disp_h) * 0.5f + pan_y_;
                    if (scale > 0.0f) {
                        int px = static_cast<int>((mp.x - img_x) / scale);
                        int py = static_cast<int>((mp.y - img_y) / scale);
                        if (px >= 0 && px < diff_tex_ws[idx] &&
                            py >= 0 && py < diff_tex_hs[idx]) {
                            hover_valid_ = true;
                            hover_cell_idx_ = idx;
                            hover_px_x_ = px;
                            hover_px_y_ = py;
                        }
                    }
                }
            }
        }
    }

    draw_selection_rect();
    draw_measurements();

    dl->PopClipRect();

    ImGui::Dummy(avail);
}

void Viewport::render_split(const std::vector<SDL_Texture*>& tex_ptrs,
                             const std::vector<int>& tex_ws,
                             const std::vector<int>& tex_hs,
                             const std::vector<const char*>& labels) {
    int n = static_cast<int>(tex_ptrs.size());
    if (n == 0) return;

    ImDrawList* dl = ImGui::GetWindowDrawList();

    int cols, rows;
    compute_grid(n, grid_layout_, grid_cols_, cols, rows);

    split_cols_ = cols;
    split_rows_ = rows;

    float cell_w = vp_size_.x / cols;
    float cell_h = vp_size_.y / rows;

    for (int i = 0; i < n; i++) {
        int col = i % cols;
        int row = i / cols;

        float cell_x = vp_origin_.x + col * cell_w;
        float cell_y = vp_origin_.y + row * cell_h;

        if (!tex_ptrs[i]) {
            ImVec2 text_size = ImGui::CalcTextSize("Empty");
            dl->AddText(ImVec2(cell_x + (cell_w - text_size.x) * 0.5f,
                               cell_y + (cell_h - text_size.y) * 0.5f),
                        IM_COL32(255, 255, 255, 80), "Empty");
            continue;
        }

        float fit_scale = std::min(cell_w / tex_ws[i], cell_h / tex_hs[i]);
        float scale = fit_scale * zoom_;
        float disp_w = tex_ws[i] * scale;
        float disp_h = tex_hs[i] * scale;

        float img_x = cell_x + (cell_w - disp_w) * 0.5f + pan_x_;
        float img_y = cell_y + (cell_h - disp_h) * 0.5f + pan_y_;

        ImVec2 img_min(img_x, img_y);
        ImVec2 img_max(img_x + disp_w, img_y + disp_h);

        dl->PushClipRect(ImVec2(cell_x, cell_y),
                         ImVec2(cell_x + cell_w, cell_y + cell_h), true);
        dl->AddImage(to_tex_id(tex_ptrs[i]), img_min, img_max);

        if (show_grid_ && scale > 0.0f) {
            draw_grid(img_min, ImVec2(disp_w, disp_h), tex_ws[i], tex_hs[i], scale);
        }

        if (labels[i]) {
            draw_image_label(labels[i],
                              img_min, ImVec2(disp_w, disp_h),
                              ImVec2(cell_x, cell_y), ImVec2(cell_w, cell_h),
                              show_ruler_);
        }
        dl->PopClipRect();

        // Rulers are drawn in the cell area (top/left strips)
        if (show_ruler_ && scale > 0.0f) {
            draw_ruler(img_min, ImVec2(disp_w, disp_h), tex_ws[i], tex_hs[i], scale,
                       ImVec2(cell_x, cell_y), ImVec2(cell_w, cell_h));
        }
    }

    // Draw cell dividers
    ImU32 divider_col = IM_COL32(255, 255, 255, 40);
    for (int c = 1; c < cols; c++) {
        float x = vp_origin_.x + c * cell_w;
        dl->AddLine(ImVec2(x, vp_origin_.y),
                    ImVec2(x, vp_origin_.y + vp_size_.y), divider_col);
    }
    for (int r = 1; r < rows; r++) {
        float y = vp_origin_.y + r * cell_h;
        dl->AddLine(ImVec2(vp_origin_.x, y),
                    ImVec2(vp_origin_.x + vp_size_.x, y), divider_col);
    }
}

void Viewport::render_overlay(const std::vector<SDL_Texture*>& tex_ptrs,
                               const std::vector<int>& tex_ws,
                               const std::vector<int>& tex_hs,
                               const std::vector<const char*>& labels) {
    int n = static_cast<int>(tex_ptrs.size());
    if (n == 0) return;

    ImDrawList* dl = ImGui::GetWindowDrawList();

    if (n == 1 || !tex_ptrs[0]) {
        if (tex_ptrs[0]) {
            ImVec2 pos, size;
            compute_image_rect(tex_ws[0], tex_hs[0], pos, size);
            dl->AddImage(to_tex_id(tex_ptrs[0]), pos,
                         ImVec2(pos.x + size.x, pos.y + size.y));
            float scale = size.x / tex_ws[0];
            if (show_grid_) draw_grid(pos, size, tex_ws[0], tex_hs[0], scale);
            if (show_ruler_) draw_ruler(pos, size, tex_ws[0], tex_hs[0], scale, vp_origin_, vp_size_);
            if (labels[0]) draw_image_label(labels[0], pos, size, vp_origin_, vp_size_, show_ruler_);
        } else {
            dl->AddText(ImVec2(vp_origin_.x + vp_size_.x * 0.5f - 60,
                               vp_origin_.y + vp_size_.y * 0.5f),
                        IM_COL32(255, 255, 255, 80), "Select images for overlay");
        }
        return;
    }

    if (!tex_ptrs[1]) {
        ImVec2 pos, size;
        compute_image_rect(tex_ws[0], tex_hs[0], pos, size);
        dl->AddImage(to_tex_id(tex_ptrs[0]), pos,
                     ImVec2(pos.x + size.x, pos.y + size.y));
        float scale = size.x / tex_ws[0];
        if (show_grid_) draw_grid(pos, size, tex_ws[0], tex_hs[0], scale);
        if (show_ruler_) draw_ruler(pos, size, tex_ws[0], tex_hs[0], scale, vp_origin_, vp_size_);
        if (labels[0]) draw_image_label(labels[0], pos, size, vp_origin_, vp_size_, show_ruler_);
        return;
    }

    // Two-image A/B slider overlay.
    //
    // tex_ws/tex_hs are the source images' native pixel sizes (e.g.
    // A=2520x1440, B=5040x2880).  The SDL textures themselves have
    // already been resampled by update_display_image so both share the
    // composite size (max of A and B); that is what makes per-pixel
    // overlay alignment well defined.  Lay the screen rect out in the
    // composite coordinate system, then sample the full texture (UV
    // 0..1) for each side.
    int img_w = std::max(tex_ws[0], tex_ws[1]);
    int img_h = std::max(tex_hs[0], tex_hs[1]);

    ImVec2 pos, size;
    compute_image_rect(img_w, img_h, pos, size);

    float line_x = vp_origin_.x + vp_size_.x * slider_pos_;

    // Draw image B (right side)
    {
        dl->PushClipRect(ImVec2(line_x, vp_origin_.y),
                         ImVec2(vp_origin_.x + vp_size_.x, vp_origin_.y + vp_size_.y), true);
        dl->AddImage(to_tex_id(tex_ptrs[1]), pos,
                     ImVec2(pos.x + size.x, pos.y + size.y));
        dl->PopClipRect();
    }

    // Draw image A (left side)
    {
        dl->PushClipRect(vp_origin_, ImVec2(line_x, vp_origin_.y + vp_size_.y), true);
        dl->AddImage(to_tex_id(tex_ptrs[0]), pos,
                     ImVec2(pos.x + size.x, pos.y + size.y));
        dl->PopClipRect();
    }

    // Grid overlay (drawn on the composite image)
    if (show_grid_ && size.x > 0.0f) {
        float scale = size.x / img_w;
        draw_grid(pos, size, img_w, img_h, scale);
    }

    // Ruler
    if (show_ruler_ && size.x > 0.0f) {
        float scale = size.x / img_w;
        draw_ruler(pos, size, img_w, img_h, scale, vp_origin_, vp_size_);
    }

    // Slider line
    {
        float vp_top = vp_origin_.y;
        float vp_bot = vp_origin_.y + vp_size_.y;
        dl->AddLine(ImVec2(line_x, vp_top),
                    ImVec2(line_x, vp_bot),
                    IM_COL32(255, 255, 255, 220), 2.0f);

        float handle_sz = 8.0f;
        dl->AddTriangleFilled(
            ImVec2(line_x - handle_sz, vp_top),
            ImVec2(line_x + handle_sz, vp_top),
            ImVec2(line_x, vp_top + handle_sz * 1.5f),
            IM_COL32(255, 255, 255, 220));
        dl->AddTriangleFilled(
            ImVec2(line_x - handle_sz, vp_bot),
            ImVec2(line_x + handle_sz, vp_bot),
            ImVec2(line_x, vp_bot - handle_sz * 1.5f),
            IM_COL32(255, 255, 255, 220));
    }

    // Slider interaction
    {
        constexpr float slider_hit_radius = 12.0f;

        float strip_x_local = line_x - vp_origin_.x - slider_hit_radius;
        if (strip_x_local < 0) strip_x_local = 0;
        float strip_w = slider_hit_radius * 2.0f;
        if (strip_x_local + strip_w > vp_size_.x)
            strip_w = vp_size_.x - strip_x_local;

        ImGui::SetCursorScreenPos(ImVec2(vp_origin_.x + strip_x_local, vp_origin_.y));
        ImGui::InvisibleButton("##overlay_slider", ImVec2(strip_w, vp_size_.y));
        slider_dragging_ = ImGui::IsItemActive();
        if (slider_dragging_ || ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }
        if (slider_dragging_) {
            float mouse_x = ImGui::GetIO().MousePos.x - vp_origin_.x;
            slider_pos_ = std::clamp(mouse_x / vp_size_.x, 0.0f, 1.0f);
        }
    }

    // Labels.  Split the composite horizontally at the slider: A owns
    // the left half, B owns the right half.  Feeding each half as the
    // label's cell rect lets draw_image_label promote the badge to the
    // area above the image when there's vertical headroom, and clamp
    // it inside its half when there isn't.
    if (labels[0]) {
        float a_w = std::max(0.0f, (line_x - vp_origin_.x));
        draw_image_label(labels[0], pos, size,
                          vp_origin_, ImVec2(a_w, vp_size_.y), show_ruler_);
    }
    if (labels[1]) {
        float slider_screen_x = vp_origin_.x + vp_size_.x * slider_pos_;
        float b_w = std::max(0.0f, vp_origin_.x + vp_size_.x - slider_screen_x);
        // The B half's image rect starts at the slider line; clip its
        // effective left edge accordingly so the badge sits inside B.
        ImVec2 b_img_pos(std::max(pos.x, slider_screen_x), pos.y);
        ImVec2 b_img_size(pos.x + size.x - b_img_pos.x, size.y);
        draw_image_label(labels[1], b_img_pos, b_img_size,
                          ImVec2(slider_screen_x, vp_origin_.y),
                          ImVec2(b_w, vp_size_.y), show_ruler_);
    }
}

void Viewport::render_difference(const std::vector<SDL_Texture*>& diff_tex_ptrs,
                                  const std::vector<int>& diff_tex_ws,
                                  const std::vector<int>& diff_tex_hs,
                                  const std::vector<const char*>& diff_labels) {
    int n = static_cast<int>(diff_tex_ptrs.size());
    if (n == 0) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddText(ImVec2(vp_origin_.x + vp_size_.x * 0.5f - 80,
                           vp_origin_.y + vp_size_.y * 0.5f - 10),
                    IM_COL32(255, 255, 255, 80), "No difference map available");
        dl->AddText(ImVec2(vp_origin_.x + vp_size_.x * 0.5f - 140,
                           vp_origin_.y + vp_size_.y * 0.5f + 10),
                    IM_COL32(255, 255, 255, 80),
                    "Select at least 2 images to compute diff against A");
        return;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Use the same grid layout as render_split so a single diff stays full-
    // size (1x1) and multi-partner diffs (A vs B, A vs C, ...) tile out
    // automatically.
    int cols, rows;
    compute_grid(n, grid_layout_, grid_cols_, cols, rows);

    split_cols_ = cols;
    split_rows_ = rows;

    float cell_w = vp_size_.x / cols;
    float cell_h = vp_size_.y / rows;

    for (int i = 0; i < n; i++) {
        int col = i % cols;
        int row = i / cols;

        float cell_x = vp_origin_.x + col * cell_w;
        float cell_y = vp_origin_.y + row * cell_h;

        if (!diff_tex_ptrs[i] || diff_tex_ws[i] <= 0 || diff_tex_hs[i] <= 0) {
            ImVec2 text_size = ImGui::CalcTextSize("(empty)");
            dl->AddText(ImVec2(cell_x + (cell_w - text_size.x) * 0.5f,
                               cell_y + (cell_h - text_size.y) * 0.5f),
                        IM_COL32(255, 255, 255, 80), "(empty)");
            continue;
        }

        float fit_scale = std::min(cell_w / diff_tex_ws[i],
                                   cell_h / diff_tex_hs[i]);
        float scale = fit_scale * zoom_;
        float disp_w = diff_tex_ws[i] * scale;
        float disp_h = diff_tex_hs[i] * scale;

        float img_x = cell_x + (cell_w - disp_w) * 0.5f + pan_x_;
        float img_y = cell_y + (cell_h - disp_h) * 0.5f + pan_y_;

        ImVec2 img_min(img_x, img_y);
        ImVec2 img_max(img_x + disp_w, img_y + disp_h);

        dl->PushClipRect(ImVec2(cell_x, cell_y),
                         ImVec2(cell_x + cell_w, cell_y + cell_h), true);
        dl->AddImage(to_tex_id(diff_tex_ptrs[i]), img_min, img_max);

        if (show_grid_ && scale > 0.0f) {
            draw_grid(img_min, ImVec2(disp_w, disp_h),
                      diff_tex_ws[i], diff_tex_hs[i], scale);
        }

        const char* label = (i < static_cast<int>(diff_labels.size()))
                                ? diff_labels[i] : nullptr;
        if (label) {
            draw_image_label(label,
                              img_min, ImVec2(disp_w, disp_h),
                              ImVec2(cell_x, cell_y), ImVec2(cell_w, cell_h),
                              show_ruler_);
        }
        dl->PopClipRect();

        if (show_ruler_ && scale > 0.0f) {
            draw_ruler(img_min, ImVec2(disp_w, disp_h),
                       diff_tex_ws[i], diff_tex_hs[i], scale,
                       ImVec2(cell_x, cell_y), ImVec2(cell_w, cell_h));
        }
    }

    // Cell dividers (match the look used in render_split).
    ImU32 divider_col = IM_COL32(255, 255, 255, 40);
    for (int c = 1; c < cols; c++) {
        float x = vp_origin_.x + c * cell_w;
        dl->AddLine(ImVec2(x, vp_origin_.y),
                    ImVec2(x, vp_origin_.y + vp_size_.y), divider_col);
    }
    for (int r = 1; r < rows; r++) {
        float y = vp_origin_.y + r * cell_h;
        dl->AddLine(ImVec2(vp_origin_.x, y),
                    ImVec2(vp_origin_.x + vp_size_.x, y), divider_col);
    }
}

void Viewport::cell_at(ImVec2 screen_pt, ImVec2& out_origin, ImVec2& out_size) const {
    float cell_w = vp_size_.x / split_cols_;
    float cell_h = vp_size_.y / split_rows_;

    int col = static_cast<int>((screen_pt.x - vp_origin_.x) / cell_w);
    int row = static_cast<int>((screen_pt.y - vp_origin_.y) / cell_h);
    col = std::clamp(col, 0, split_cols_ - 1);
    row = std::clamp(row, 0, split_rows_ - 1);

    out_origin = ImVec2(vp_origin_.x + col * cell_w,
                        vp_origin_.y + row * cell_h);
    out_size = ImVec2(cell_w, cell_h);
}

} // namespace idiff
