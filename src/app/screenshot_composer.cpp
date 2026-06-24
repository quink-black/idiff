// Implementation of compose_viewport().  Logic lifted verbatim from
// the previous body of App::save_viewport_dialog(); the only change
// is that I/O (file dialog, status reporter) and viewport-mode lookup
// have been hoisted to the caller.

#include "app/screenshot_composer.h"

#include "app/viewport.h"
#include "domain/diff_service.h"
#include "core/image.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>

namespace idiff {

namespace {

// Convert any incoming Mat (8/16-bit, 1/3/4 channels, RGB(A) byte
// order) to BGRA-8.  Returns an empty Mat for unsupported channel
// counts.
cv::Mat to_bgra8(const cv::Mat& src) {
    cv::Mat s = src;
    if (s.depth() != CV_8U) {
        // Map [0, typeMax] -> [0, 255] so the saved image matches what
        // the user sees on screen (textures are uploaded 8-bit).
        double scale = (s.depth() == CV_16U) ? (1.0 / 257.0) : 1.0;
        s.convertTo(s, CV_8U, scale);
    }
    cv::Mat out;
    switch (s.channels()) {
        case 1: cv::cvtColor(s, out, cv::COLOR_GRAY2BGRA); break;
        // In-memory image mats are RGB/RGBA; convert to BGR/BGRA so
        // cv::imwrite produces a correct file.
        case 3: cv::cvtColor(s, out, cv::COLOR_RGB2BGRA); break;
        case 4: cv::cvtColor(s, out, cv::COLOR_RGBA2BGRA); break;
        default: return {};
    }
    return out;
}

void set_error(std::string* sink, const char* msg) {
    if (sink) *sink = msg;
}

} // namespace

cv::Mat compose_viewport(const ComposeViewportInput& in,
                         std::string* error_message) {
    if (!in.entries || !in.selection) {
        set_error(error_message, "compose_viewport: entries/selection missing");
        return {};
    }
    const auto& entries = *in.entries;
    const auto& selection = *in.selection;

    auto entry_display_mat = [&](int idx) -> cv::Mat {
        if (idx < 0 || idx >= static_cast<int>(entries.size())) return {};
        const auto& e = entries[idx];
        const Image* img = e.display_image ? e.display_image.get()
                                           : e.image.get();
        if (!img) return {};
        return img->mat();
    };

    // Gather the ordered list of mats in the same order push_entry()
    // populates for the viewport (Ref first, then the remaining
    // selected entries in natural order).
    std::vector<cv::Mat> slot_mats;
    auto push_slot = [&](int idx) {
        cv::Mat m = entry_display_mat(idx);
        if (m.empty()) return;
        slot_mats.push_back(m);
    };
    if (in.ref_idx >= 0) push_slot(in.ref_idx);
    for (int s : selection) {
        if (s == in.ref_idx) continue;
        push_slot(s);
    }

    if (slot_mats.empty() &&
        !(in.mode == ComparisonMode::Difference
          && in.diff && !in.diff->empty())) {
        set_error(error_message, "nothing to save (no media selected)");
        return {};
    }

    cv::Mat composed;

    if (in.mode == ComparisonMode::Difference) {
        if (!in.diff || in.diff->empty()) {
            set_error(error_message,
                      "no diff map available (select at least 2 media)");
            return {};
        }

        int n = static_cast<int>(in.diff->size());
        int cols = 1, rows = 1;
        Viewport::compute_grid(n, in.grid_layout, in.grid_cols, cols, rows);

        int cell_w = 0, cell_h = 0;
        for (const auto& slot : in.diff->slots()) {
            if (!slot.image) continue;
            cell_w = std::max(cell_w, slot.image->mat().cols);
            cell_h = std::max(cell_h, slot.image->mat().rows);
        }
        if (cell_w <= 0 || cell_h <= 0) {
            set_error(error_message, "diff image has zero dimensions");
            return {};
        }

        int out_w = cell_w * cols;
        int out_h = cell_h * rows;
        cv::Mat canvas = cv::Mat::zeros(out_h, out_w, CV_8UC4);

        for (int i = 0; i < n; i++) {
            if (!in.diff->slots()[i].image) continue;
            cv::Mat m = to_bgra8(in.diff->slots()[i].image->mat());
            if (m.empty()) continue;
            int col = i % cols;
            int row = i / cols;
            int x = col * cell_w + (cell_w - m.cols) / 2;
            int y = row * cell_h + (cell_h - m.rows) / 2;
            m.copyTo(canvas(cv::Rect(x, y, m.cols, m.rows)));
        }

        cv::Scalar divider(255, 255, 255, 80);
        for (int c = 1; c < cols; c++) {
            cv::line(canvas, {c * cell_w, 0},
                     {c * cell_w, out_h - 1}, divider, 1);
        }
        for (int r = 1; r < rows; r++) {
            cv::line(canvas, {0, r * cell_h},
                     {out_w - 1, r * cell_h}, divider, 1);
        }
        composed = canvas;
    } else if (in.mode == ComparisonMode::Overlay) {
        // Reproduce the viewport's A/B slider.  The slider is anchored
        // to the viewport, so in image-pixel space the split column is
        // just slider_pos * composite_width.
        cv::Mat a = slot_mats.size() >= 1 ? to_bgra8(slot_mats[0])
                                          : cv::Mat();
        cv::Mat b = slot_mats.size() >= 2 ? to_bgra8(slot_mats[1])
                                          : cv::Mat();
        if (a.empty() && b.empty()) {
            set_error(error_message, "no images for overlay");
            return {};
        }
        if (b.empty()) {
            composed = a;  // only A selected
        } else {
            int w = std::max(a.cols, b.cols);
            int h = std::max(a.rows, b.rows);
            cv::Mat canvas = cv::Mat::zeros(h, w, CV_8UC4);

            int split = std::clamp(
                static_cast<int>(std::round(in.overlay_slider_pos * w)),
                0, w);

            // Left half from A, right half from B.  display_image is
            // already upscaled to the common size, but guard just in
            // case.
            auto blit = [](const cv::Mat& src, cv::Mat& dst,
                           int x0, int x1) {
                if (src.empty() || x1 <= x0) return;
                int sw = std::min(src.cols, x1) - x0;
                if (sw <= 0) return;
                int sh = std::min(src.rows, dst.rows);
                cv::Rect src_roi(x0, 0, sw, sh);
                cv::Rect dst_roi(x0, 0, sw, sh);
                if (x0 >= src.cols) return;
                src(src_roi).copyTo(dst(dst_roi));
            };
            blit(a, canvas, 0, split);
            blit(b, canvas, split, w);

            // Draw a thin vertical divider so the split is obvious in
            // the saved image.
            if (split > 0 && split < w) {
                cv::line(canvas, {split, 0}, {split, h - 1},
                         cv::Scalar(255, 255, 255, 255), 1);
            }
            composed = canvas;
        }
    } else {  // Split
        int n = static_cast<int>(slot_mats.size());
        if (n == 0) {
            set_error(error_message, "no images to save");
            return {};
        }
        int cols = 1, rows = 1;
        Viewport::compute_grid(n, in.grid_layout, in.grid_cols, cols, rows);

        // Use the largest image size as the per-cell size so cells
        // stay uniform; smaller images are centered with transparent
        // padding.
        int cell_w = 0, cell_h = 0;
        for (const auto& m : slot_mats) {
            cell_w = std::max(cell_w, m.cols);
            cell_h = std::max(cell_h, m.rows);
        }
        if (cell_w <= 0 || cell_h <= 0) {
            set_error(error_message, "image has zero dimensions");
            return {};
        }

        int out_w = cell_w * cols;
        int out_h = cell_h * rows;
        cv::Mat canvas = cv::Mat::zeros(out_h, out_w, CV_8UC4);

        for (int i = 0; i < n; i++) {
            int col = i % cols;
            int row = i / cols;
            cv::Mat m = to_bgra8(slot_mats[i]);
            if (m.empty()) continue;
            int x = col * cell_w + (cell_w - m.cols) / 2;
            int y = row * cell_h + (cell_h - m.rows) / 2;
            m.copyTo(canvas(cv::Rect(x, y, m.cols, m.rows)));
        }

        cv::Scalar divider(255, 255, 255, 80);
        for (int c = 1; c < cols; c++) {
            cv::line(canvas, {c * cell_w, 0},
                     {c * cell_w, out_h - 1}, divider, 1);
        }
        for (int r = 1; r < rows; r++) {
            cv::line(canvas, {0, r * cell_h},
                     {out_w - 1, r * cell_h}, divider, 1);
        }
        composed = canvas;
    }

    if (composed.empty()) {
        set_error(error_message, "failed to compose viewport image");
        return {};
    }
    return composed;
}

} // namespace idiff
