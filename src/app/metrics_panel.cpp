#include "app/metrics_panel.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <unordered_set>

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

void MetricsPanel::render_statistics(
        const std::vector<std::pair<std::string, const Image*>>& images) {
    if (images.empty()) {
        ImGui::TextDisabled("No images selected");
        return;
    }

    // Prune cache entries for images that are no longer visible.  This keeps
    // the cache bounded in long sessions where many different images cycle
    // through the statistics panel.
    {
        std::unordered_set<const Image*> live;
        live.reserve(images.size());
        for (const auto& [_, img] : images) {
            if (img) live.insert(img);
        }
        for (auto it = stats_cache_.begin(); it != stats_cache_.end();) {
            if (live.find(it->first) == live.end()) {
                it = stats_cache_.erase(it);
            } else {
                ++it;
            }
        }
        // Defensive upper bound in case `images` itself is pathological.
        while (stats_cache_.size() > kMaxCacheEntries) {
            stats_cache_.erase(stats_cache_.begin());
        }
    }

    ImGui::Checkbox("Show Histogram", &hist_show_);

    ImGui::Separator();

    // Scrollable area for potentially many images
    ImGui::BeginChild("##stats_scroll", ImVec2(0, 0), false, ImGuiWindowFlags_None);

    for (auto& [name, img] : images) {
        auto& cache = stats_cache_[img];
        render_image_stats(name, img, cache);
    }

    ImGui::EndChild();
}

void MetricsPanel::render_image_stats(const std::string& name,
                                       const Image* image,
                                       PerImageStats& cache) {
    if (!image) {
        ImGui::TextDisabled("%s: (not loaded)", name.c_str());
        return;
    }

    // Collapsible header per image
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen;
    if (!ImGui::CollapsingHeader(name.c_str(), flags)) {
        return;
    }

    // Compute button for this image
    ImGui::PushID(name.c_str());
    if (ImGui::SmallButton("Compute")) {
        MetricsEngine engine;
        auto result = engine.compute_single(*image);
        if (result) {
            cache.metrics = *result;
            cache.computed = true;
        }
        if (hist_show_) {
            auto hist = engine.compute_histogram(*image);
            if (hist) {
                cache.histogram = std::move(*hist);
                cache.hist_valid = true;
            }
        }
    }
    ImGui::PopID();

    if (!cache.computed) {
        ImGui::TextDisabled("  Click 'Compute' to analyze");
        ImGui::Separator();
        return;
    }

    const auto& m = cache.metrics;

    // Stats table: Channel | Mean | Std | Min | Max
    if (ImGui::BeginTable("##stats_table", 5,
                          ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Ch", ImGuiTableColumnFlags_WidthFixed, 30.0f);
        ImGui::TableSetupColumn("Mean", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Std", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Min", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Max", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        auto row = [&](const char* label, double mean, double var,
                       double mn, double mx) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(label);
            ImGui::TableNextColumn(); ImGui::Text("%.1f", mean);
            ImGui::TableNextColumn(); ImGui::Text("%.1f", std::sqrt(var));
            ImGui::TableNextColumn(); ImGui::Text("%.0f", mn);
            ImGui::TableNextColumn(); ImGui::Text("%.0f", mx);
        };

        row("R", m.mean_r, m.var_r, m.min_r, m.max_r);
        row("G", m.mean_g, m.var_g, m.min_g, m.max_g);
        row("B", m.mean_b, m.var_b, m.min_b, m.max_b);

        ImGui::EndTable();
    }

    // Histogram
    if (hist_show_ && cache.hist_valid) {
        draw_histogram(cache.histogram, name.c_str());
    }

    ImGui::Separator();
}

void MetricsPanel::draw_histogram(const Histogram& hist, const char* title) {
    uint32_t max_val = 1;
    for (int i = 0; i < 256; i++) {
        max_val = std::max({max_val, hist.r[i], hist.g[i], hist.b[i]});
    }

    ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
    float canvas_w = ImGui::GetContentRegionAvail().x;
    float canvas_h = 80.0f;

    ImDrawList* dl = ImGui::GetWindowDrawList();

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

    draw_channel(hist.r, IM_COL32(255, 80, 80, 160));
    draw_channel(hist.g, IM_COL32(80, 255, 80, 160));
    draw_channel(hist.b, IM_COL32(80, 120, 255, 160));

    ImGui::Dummy(ImVec2(canvas_w, canvas_h));
}

void MetricsPanel::invalidate_cache() {
    stats_cache_.clear();
}

} // namespace idiff
