#include "app/metrics_panel.h"

#include <imgui.h>

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

} // namespace idiff
