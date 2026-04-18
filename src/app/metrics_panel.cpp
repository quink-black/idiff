#include "app/metrics_panel.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/image.h"
#include "core/metrics_engine.h"

namespace idiff {

MetricsPanel::MetricsPanel() = default;
MetricsPanel::~MetricsPanel() = default;

void MetricsPanel::render(const Image* image_a, const Image* image_b) {
    ImGui::SetNextWindowSize(ImVec2(300, 400), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Metrics")) {
        ImGui::End();
        return;
    }
    render_inline(image_a, image_b);
    ImGui::End();
}

void MetricsPanel::render_inline(const Image* image_a, const Image* image_b) {
    if (!image_a || !image_b) {
        ImGui::TextDisabled("Load both images to compute metrics");
        return;
    }

    if (ImGui::Button("Compute Metrics", ImVec2(-1, 0))) {
        MetricsEngine engine;
        auto result = engine.compute(*image_a, *image_b);
        if (result) {
            psnr_ = result->psnr;
            ssim_ = result->ssim;
            mse_ = result->mse;
            metrics_computed_ = true;
        }
    }

    ImGui::Separator();

    if (metrics_computed_) {
        // Table layout for clean metric display
        if (ImGui::BeginTable("##metrics_table", 2, ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("Metric", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextRow();

            ImGui::TableNextColumn(); ImGui::TextUnformatted("PSNR");
            ImGui::TableNextColumn(); ImGui::Text("%.2f dB", psnr_);

            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted("SSIM");
            ImGui::TableNextColumn(); ImGui::Text("%.4f", ssim_);

            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted("MSE");
            ImGui::TableNextColumn(); ImGui::Text("%.2f", mse_);

            ImGui::EndTable();
        }
    } else {
        ImGui::TextDisabled("Click 'Compute Metrics' to analyze");
    }
}

void MetricsPanel::render_single(const Image* image) {
    if (!image) {
        ImGui::TextDisabled("No image selected");
        return;
    }

    if (ImGui::Button("Compute Statistics", ImVec2(-1, 0))) {
        MetricsEngine engine;
        auto result = engine.compute_single(*image);
        if (result) {
            mean_r_ = result->mean_r;
            mean_g_ = result->mean_g;
            mean_b_ = result->mean_b;
            var_r_  = result->var_r;
            var_g_  = result->var_g;
            var_b_  = result->var_b;
            single_computed_ = true;
        }
    }

    ImGui::Separator();

    if (single_computed_) {
        if (ImGui::BeginTable("##single_metrics_table", 4,
                              ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Channel", ImGuiTableColumnFlags_WidthFixed, 55.0f);
            ImGui::TableSetupColumn("Mean", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Var", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Std", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            auto row = [&](const char* label, double mean, double var) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(label);
                ImGui::TableNextColumn(); ImGui::Text("%.2f", mean);
                ImGui::TableNextColumn(); ImGui::Text("%.2f", var);
                ImGui::TableNextColumn(); ImGui::Text("%.2f", std::sqrt(var));
            };

            row("R", mean_r_, var_r_);
            row("G", mean_g_, var_g_);
            row("B", mean_b_, var_b_);

            ImGui::EndTable();
        }
    }

    // --- Histogram ---
    ImGui::Separator();

    ImGui::Checkbox("Show Histogram", &hist_show_);
    if (hist_show_) {
        if (ImGui::Button("Compute Histogram", ImVec2(-1, 0))) {
            MetricsEngine engine;
            auto hist = engine.compute_histogram(*image);
            if (hist) {
                cached_hist_ = std::move(*hist);
                hist_cache_valid_ = true;
            }
        }

        if (hist_cache_valid_) {
            draw_histogram(cached_hist_);
        }
    }
}

void MetricsPanel::draw_histogram(const Histogram& hist) {
    // Find the max value across all channels for normalisation
    uint32_t max_val = 1;
    for (int i = 0; i < 256; i++) {
        max_val = std::max({max_val, hist.r[i], hist.g[i], hist.b[i]});
    }

    ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
    float canvas_w = ImGui::GetContentRegionAvail().x;
    float canvas_h = 100.0f;

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Background
    dl->AddRectFilled(canvas_pos,
                      ImVec2(canvas_pos.x + canvas_w, canvas_pos.y + canvas_h),
                      IM_COL32(30, 30, 30, 255));

    float bar_w = canvas_w / 256.0f;

    auto draw_channel = [&](const std::array<uint32_t, 256>& data, ImU32 color) {
        for (int i = 0; i < 256; i++) {
            float h = (static_cast<float>(data[i]) / max_val) * canvas_h;
            float x = canvas_pos.x + i * bar_w;
            dl->AddRectFilled(
                ImVec2(x, canvas_pos.y + canvas_h - h),
                ImVec2(x + bar_w, canvas_pos.y + canvas_h),
                color);
        }
    };

    draw_channel(hist.r, IM_COL32(255, 80, 80, 160));   // Red
    draw_channel(hist.g, IM_COL32(80, 255, 80, 160));   // Green
    draw_channel(hist.b, IM_COL32(80, 120, 255, 160));  // Blue

    // Reserve the space so ImGui knows we drew something
    ImGui::Dummy(ImVec2(canvas_w, canvas_h));
}

} // namespace idiff
