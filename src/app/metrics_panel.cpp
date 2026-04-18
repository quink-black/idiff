#include "app/metrics_panel.h"

#include <imgui.h>

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
}

} // namespace idiff
