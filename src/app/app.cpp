#include "app/app.h"
#include "app/viewport.h"
#include "app/metrics_panel.h"
#include "app/properties_panel.h"

#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>
#include <imgui_internal.h>
#include <SDL.h>
#include <nfd.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <unordered_map>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "core/image_loader.h"
#include "core/image_processor.h"
#include "core/image_comparator.h"

namespace idiff {

struct App::State {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;

    std::unique_ptr<Viewport> viewport;
    std::unique_ptr<MetricsPanel> metrics_panel;
    std::unique_ptr<PropertiesPanel> properties_panel;

    UpscaleMethod upscale_method = UpscaleMethod::Lanczos;

    std::string status_text;
    bool show_metrics = true;
    bool show_properties = true;
    bool show_image_list = true;
    int sidebar_tab = 0;
};

App::App() : state_(std::make_unique<State>()) {}

App::~App() = default;

bool App::init(SDL_Window* window, SDL_Renderer* renderer) {
    state_->window = window;
    state_->renderer = renderer;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.GrabRounding = 2.0f;
    style.WindowPadding = ImVec2(8, 8);
    style.FramePadding = ImVec2(6, 3);
    style.ItemSpacing = ImVec2(6, 4);
    style.TabRounding = 4.0f;
    style.DockingSeparatorSize = 3.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.12f, 0.12f, 0.18f, 1.00f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.10f, 0.10f, 0.15f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.16f, 0.16f, 0.24f, 1.00f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.18f, 0.18f, 0.26f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.24f, 0.24f, 0.34f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.28f, 0.28f, 0.40f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.29f, 0.56f, 0.85f, 0.40f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.29f, 0.62f, 1.00f, 0.60f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.29f, 0.62f, 1.00f, 0.80f);
    colors[ImGuiCol_Header] = ImVec4(0.29f, 0.62f, 1.00f, 0.30f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.29f, 0.62f, 1.00f, 0.45f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.29f, 0.62f, 1.00f, 0.55f);
    colors[ImGuiCol_Tab] = ImVec4(0.16f, 0.16f, 0.24f, 1.00f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.29f, 0.62f, 1.00f, 0.60f);
    colors[ImGuiCol_TabActive] = ImVec4(0.24f, 0.48f, 0.74f, 1.00f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.29f, 0.62f, 1.00f, 1.00f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.29f, 0.62f, 1.00f, 0.80f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.40f, 0.72f, 1.00f, 1.00f);
    colors[ImGuiCol_TextSelectedBg] = ImVec4(0.29f, 0.62f, 1.00f, 0.35f);
    colors[ImGuiCol_DockingPreview] = ImVec4(0.29f, 0.62f, 1.00f, 0.70f);
    colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.07f, 0.07f, 0.10f, 1.00f);

    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    int fb_w = 0, fb_h = 0;
    SDL_GetRendererOutputSize(renderer, &fb_w, &fb_h);
    int win_w = 0, win_h = 0;
    SDL_GetWindowSize(window, &win_w, &win_h);
    float dpi_scale = (win_w > 0) ? static_cast<float>(fb_w) / win_w : 1.0f;
    if (dpi_scale < 1.0f) dpi_scale = 1.0f;
    SDL_RenderSetScale(renderer, dpi_scale, dpi_scale);

    state_->viewport = std::make_unique<Viewport>();
    state_->metrics_panel = std::make_unique<MetricsPanel>();
    state_->properties_panel = std::make_unique<PropertiesPanel>();

    NFD_Init();

    return true;
}

void App::shutdown() {
    NFD_Quit();

    for (auto& entry : entries_) {
        if (entry.texture) {
            SDL_DestroyTexture(entry.texture);
            entry.texture = nullptr;
        }
    }
    entries_.clear();

    if (diff_texture_.texture) {
        SDL_DestroyTexture(diff_texture_.texture);
        diff_texture_.texture = nullptr;
    }
    diff_image_.reset();

    state_->viewport.reset();
    state_->metrics_panel.reset();
    state_->properties_panel.reset();

    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
}

void App::frame() {
    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    render_toolbar();

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowViewport(vp->ID);

    ImGuiWindowFlags dock_flags = ImGuiWindowFlags_NoDocking |
                                   ImGuiWindowFlags_NoTitleBar |
                                   ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus |
                                   ImGuiWindowFlags_NoNavFocus |
                                   ImGuiWindowFlags_MenuBar;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##DockSpace", nullptr, dock_flags);
    ImGui::PopStyleVar(3);

    ImGuiID dockspace_id = ImGui::GetID("MainDockSpace");
    if (first_frame_) {
        setup_dock_layout();
        first_frame_ = false;
    }
    // NoWindowMenuButton hides the small arrow ImGui normally draws in the
    // lower-left corner of the central dock node (the per-node window menu).
    // It is not useful in this app and only confuses users.
    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f),
                     ImGuiDockNodeFlags_NoWindowMenuButton);

    ImGui::End();

    render_image_list();
    render_viewport();
    render_right_sidebar();
    render_status_bar();

    ImGui::Render();

    SDL_SetRenderDrawColor(state_->renderer, 18, 18, 26, 255);
    SDL_RenderClear(state_->renderer);
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), state_->renderer);
    SDL_RenderPresent(state_->renderer);
}

void App::setup_dock_layout() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGuiID dockspace_id = ImGui::GetID("MainDockSpace");

    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, vp->WorkSize);

    ImGuiID dock_left, dock_center_right;
    ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Left, 0.20f, &dock_left, &dock_center_right);

    ImGuiID dock_center, dock_right;
    ImGui::DockBuilderSplitNode(dock_center_right, ImGuiDir_Right, 0.25f, &dock_right, &dock_center);

    ImGui::DockBuilderDockWindow("Images", dock_left);
    ImGui::DockBuilderDockWindow("Viewport", dock_center);
    ImGui::DockBuilderDockWindow("Inspector", dock_right);

    ImGui::DockBuilderFinish(dockspace_id);
}

void App::load_images(const std::vector<std::string>& paths) {
    ImageLoader loader;
    for (const auto& path : paths) {
        auto img = loader.load(path);
        if (img) {
            ImageEntry entry;
            entry.path = path;
            auto sep = path.find_last_of("/\\");
            entry.filename = (sep != std::string::npos) ? path.substr(sep + 1) : path;
            entry.display_label = entry.filename;
            entry.image = std::move(img);
            entry.display_image = nullptr;
            entry.texture = nullptr;
            entry.texture_dirty = true;

            entries_.push_back(std::move(entry));
            state_->status_text = "Loaded: " + path;
        } else {
            state_->status_text = "Failed to load: " + path + " (" + loader.last_error() + ")";
        }
    }

    sort_entries_by_name();
    compute_display_labels();
    diff_texture_.dirty = true;
}

void App::get_ab_indices(int& a_idx, int& b_idx) const {
    a_idx = -1;
    b_idx = -1;
    int k = 0;
    for (int s : selected_) {
        if (k == 0) a_idx = s;
        else if (k == 1) { b_idx = s; break; }
        k++;
    }
    if (swap_ab_) std::swap(a_idx, b_idx);
}

void App::sort_entries_by_name() {
    // Build a mapping from old index to entry pointer for selected_ fixup
    std::vector<int> old_indices(entries_.size());
    std::iota(old_indices.begin(), old_indices.end(), 0);

    // Sort entries and track the permutation
    std::sort(old_indices.begin(), old_indices.end(), [&](int a, int b) {
        return entries_[a].filename < entries_[b].filename;
    });

    // Apply permutation
    std::vector<ImageEntry> sorted;
    sorted.reserve(entries_.size());
    for (int idx : old_indices) {
        sorted.push_back(std::move(entries_[idx]));
    }

    // Remap selected_ indices
    std::vector<int> remap(entries_.size());
    for (int i = 0; i < static_cast<int>(old_indices.size()); i++) {
        remap[old_indices[i]] = i;
    }
    std::set<int> new_selected;
    for (int s : selected_) {
        if (s >= 0 && s < static_cast<int>(remap.size())) {
            new_selected.insert(remap[s]);
        }
    }
    selected_ = std::move(new_selected);
    entries_ = std::move(sorted);
}

void App::move_entry(int from, int to) {
    if (from == to) return;
    if (from < 0 || from >= static_cast<int>(entries_.size())) return;
    if (to < 0 || to >= static_cast<int>(entries_.size())) return;

    // Build old-to-new index mapping
    std::vector<int> remap(entries_.size());
    std::iota(remap.begin(), remap.end(), 0);

    // Move the entry
    ImageEntry tmp = std::move(entries_[from]);
    if (from < to) {
        for (int i = from; i < to; i++) {
            entries_[i] = std::move(entries_[i + 1]);
            remap[i + 1] = i;
        }
    } else {
        for (int i = from; i > to; i--) {
            entries_[i] = std::move(entries_[i - 1]);
            remap[i - 1] = i;
        }
    }
    entries_[to] = std::move(tmp);
    remap[from] = to;

    // Remap selected_ indices
    std::set<int> new_selected;
    for (int s : selected_) {
        if (s >= 0 && s < static_cast<int>(remap.size())) {
            new_selected.insert(remap[s]);
        }
    }
    selected_ = std::move(new_selected);
}

void App::open_file_dialog() {
    nfdfilteritem_t filters[] = {{ "Image files", "png,jpg,jpeg,bmp,tiff,tif,webp,dng,cr2,nef,arw" }};
    const nfdpathset_t* outPaths = nullptr;
    nfdresult_t result = NFD_OpenDialogMultiple(&outPaths, filters, 1, nullptr);
    if (result == NFD_OKAY) {
        nfdpathsetsize_t count = 0;
        NFD_PathSet_GetCount(outPaths, &count);
        std::vector<std::string> paths;
        for (nfdpathsetsize_t i = 0; i < count; i++) {
            nfdchar_t* path = nullptr;
            NFD_PathSet_GetPath(outPaths, i, &path);
            if (path) {
                paths.emplace_back(path);
                NFD_PathSet_FreePath(path);
            }
        }
        NFD_PathSet_Free(outPaths);
        load_images(paths);
    } else if (result == NFD_ERROR) {
        state_->status_text = "File dialog error: " + std::string(NFD_GetError());
    }
}

// Compose the current viewport contents into a single BGRA image and write
// it to disk.  The output is in image-pixel space (not window pixels), so
// zoom / pan does not affect quality; the user always gets a full-resolution
// snapshot of what the three comparison modes depict.
void App::save_viewport_dialog() {
    auto& vport = *state_->viewport;
    ComparisonMode mode = vport.mode();

    // --- Collect the input images that feed the viewport ---
    int ab_idx[2] = {-1, -1};
    get_ab_indices(ab_idx[0], ab_idx[1]);

    auto entry_display_mat = [&](int idx) -> cv::Mat {
        if (idx < 0 || idx >= static_cast<int>(entries_.size())) return {};
        const auto& e = entries_[idx];
        const Image* img = e.display_image ? e.display_image.get()
                                           : e.image.get();
        if (!img) return {};
        return img->mat();
    };

    // Gather the ordered list of mats in the same order push_entry()
    // populates for the viewport (A, B, then the remaining selected).
    std::vector<cv::Mat> slot_mats;
    std::vector<std::string> slot_labels;
    auto push_slot = [&](int idx, const char* tag) {
        cv::Mat m = entry_display_mat(idx);
        if (m.empty()) return;
        slot_mats.push_back(m);
        slot_labels.push_back(
            tag ? (std::string("[") + tag + "] " + entries_[idx].display_label)
                : entries_[idx].display_label);
    };
    if (ab_idx[0] >= 0) push_slot(ab_idx[0], "A");
    if (ab_idx[1] >= 0) push_slot(ab_idx[1], "B");
    for (int s : selected_) {
        if (s == ab_idx[0] || s == ab_idx[1]) continue;
        push_slot(s, nullptr);
    }

    if (slot_mats.empty() &&
        !(mode == ComparisonMode::Difference && diff_image_)) {
        state_->status_text = "Save: nothing to save (no images selected)";
        return;
    }

    // Normalize every mat to BGRA-8 so we can compose freely without per-cell
    // depth/channel branching.  Saving in 8-bit is fine here because the
    // viewport itself renders via 8-bit SDL textures.
    auto to_bgra8 = [](const cv::Mat& src) -> cv::Mat {
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
    };

    cv::Mat composed;  // final image to write (BGRA-8)

    if (mode == ComparisonMode::Difference) {
        if (!diff_image_) {
            state_->status_text = "Save: no diff map available "
                                  "(select exactly 2 images first)";
            return;
        }
        composed = to_bgra8(diff_image_->mat());
    } else if (mode == ComparisonMode::Overlay) {
        // Reproduce the viewport's A/B slider.  The slider is anchored to
        // the viewport, so in image-pixel space the split column is just
        // slider_pos * composite_width.
        cv::Mat a = slot_mats.size() >= 1 ? to_bgra8(slot_mats[0]) : cv::Mat();
        cv::Mat b = slot_mats.size() >= 2 ? to_bgra8(slot_mats[1]) : cv::Mat();
        if (a.empty() && b.empty()) {
            state_->status_text = "Save: no images for overlay";
            return;
        }
        if (b.empty()) {
            composed = a;  // only A selected
        } else {
            int w = std::max(a.cols, b.cols);
            int h = std::max(a.rows, b.rows);
            cv::Mat canvas = cv::Mat::zeros(h, w, CV_8UC4);

            float slider = vport.overlay_slider_pos();
            int split = std::clamp(static_cast<int>(std::round(slider * w)),
                                   0, w);

            // Left half from A, right half from B.  display_image is already
            // upscaled to the common size, but guard just in case.
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

            // Draw a thin vertical divider so the split is obvious in the
            // saved image.
            if (split > 0 && split < w) {
                cv::line(canvas, {split, 0}, {split, h - 1},
                         cv::Scalar(255, 255, 255, 255), 1);
            }
            composed = canvas;
        }
    } else {  // Split
        // Match the grid layout Viewport::render_split uses.
        int n = static_cast<int>(slot_mats.size());
        if (n == 0) {
            state_->status_text = "Save: no images to save";
            return;
        }
        int cols, rows;
        if (n == 1) { cols = 1; rows = 1; }
        else if (n == 2) { cols = 2; rows = 1; }
        else if (n <= 4) { cols = 2; rows = 2; }
        else if (n <= 6) { cols = 3; rows = 2; }
        else { cols = 3; rows = (n + cols - 1) / cols; }

        // Use the largest image size as the per-cell size so cells stay
        // uniform; smaller images are centered with transparent padding.
        int cell_w = 0, cell_h = 0;
        for (const auto& m : slot_mats) {
            cell_w = std::max(cell_w, m.cols);
            cell_h = std::max(cell_h, m.rows);
        }
        if (cell_w <= 0 || cell_h <= 0) {
            state_->status_text = "Save: image has zero dimensions";
            return;
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

        // Draw cell dividers (matching the white-translucent look on screen).
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
        state_->status_text = "Save: failed to compose viewport image";
        return;
    }

    // --- Ask the user for a destination path ---
    nfdfilteritem_t filters[] = {
        { "PNG image", "png" },
        { "JPEG image", "jpg,jpeg" },
    };
    const char* default_name = "viewport.png";
    nfdchar_t* out_path = nullptr;
    nfdresult_t result = NFD_SaveDialog(&out_path, filters, 2, nullptr,
                                         default_name);
    if (result != NFD_OKAY) {
        if (result == NFD_ERROR) {
            state_->status_text = "Save dialog error: " +
                                  std::string(NFD_GetError());
        }
        return;
    }
    std::string path = out_path;
    NFD_FreePath(out_path);

    // NFD_SaveDialog does not always append an extension; default to .png
    // when none was given so cv::imwrite picks the right encoder.
    auto has_ext = [](const std::string& p) {
        auto dot = p.find_last_of('.');
        auto slash = p.find_last_of("/\\");
        return dot != std::string::npos &&
               (slash == std::string::npos || dot > slash);
    };
    if (!has_ext(path)) path += ".png";

    // Convert back to the byte order cv::imwrite expects (BGR/BGRA).  Our
    // `composed` is already BGRA so no further conversion is needed.
    try {
        bool ok = cv::imwrite(path, composed);
        state_->status_text = ok ? ("Saved viewport to: " + path)
                                 : ("Save failed: " + path);
    } catch (const cv::Exception& ex) {
        state_->status_text = std::string("Save failed: ") + ex.what();
    }
}

void App::remove_entry(int index) {
    if (index < 0 || index >= static_cast<int>(entries_.size())) return;

    if (entries_[index].texture) {
        SDL_DestroyTexture(entries_[index].texture);
    }
    entries_.erase(entries_.begin() + index);

    std::set<int> new_selected;
    for (int s : selected_) {
        if (s == index) continue;
        if (s > index) new_selected.insert(s - 1);
        else new_selected.insert(s);
    }
    // Membership changed -- reset A/B swap so it doesn't apply to a different pair.
    if (new_selected != selected_) {
        swap_ab_ = false;
    }
    selected_ = std::move(new_selected);

    compute_display_labels();
    diff_texture_.dirty = true;
}

void App::compute_display_labels() {
    if (entries_.empty()) return;

    std::unordered_map<std::string, int> name_counts;
    for (const auto& entry : entries_) {
        name_counts[entry.filename]++;
    }

    for (auto& entry : entries_) {
        if (name_counts[entry.filename] > 1) {
            auto sep = entry.path.find_last_of("/\\");
            if (sep != std::string::npos) {
                auto parent = entry.path.substr(0, sep);
                auto sep2 = parent.find_last_of("/\\");
                std::string dir_name = (sep2 != std::string::npos)
                    ? parent.substr(sep2 + 1) : parent;
                entry.display_label = dir_name + "/" + entry.filename;
            } else {
                entry.display_label = entry.filename;
            }
        } else {
            entry.display_label = entry.filename;
        }
    }
}

void App::update_display_image(int index) {
    if (index < 0 || index >= static_cast<int>(entries_.size())) return;

    auto& entry = entries_[index];
    if (!entry.image) return;

    int target_w = entry.image->info().width;
    int target_h = entry.image->info().height;

    for (int s : selected_) {
        if (s == index) continue;
        if (s < 0 || s >= static_cast<int>(entries_.size())) continue;
        const auto& other = entries_[s];
        if (other.image) {
            target_w = std::max(target_w, other.image->info().width);
            target_h = std::max(target_h, other.image->info().height);
        }
    }

    bool needs_upscale = entry.image->info().width < target_w ||
                         entry.image->info().height < target_h;

    if (needs_upscale) {
        ImageProcessor proc;
        UpscaleOptions opts;
        opts.target_width = target_w;
        opts.target_height = target_h;
        opts.method = state_->upscale_method;
        entry.display_image = proc.upscale(*entry.image, opts);
    } else {
        // No upscale needed — clear any stale display_image from a previous comparison
        entry.display_image.reset();
    }

    entry.texture_dirty = true;
    diff_texture_.dirty = true;
}

void App::upload_texture(ImageEntry& entry) {
    const Image* img = entry.display_image ? entry.display_image.get() : entry.image.get();
    if (!img) return;

    const auto& mat = img->mat();
    if (mat.empty()) return;

    int w = mat.cols;
    int h = mat.rows;
    int channels = mat.channels();

    // Metal backend does not reliably support SDL_PIXELFORMAT_RGB24 (3-byte).
    // Always convert to RGBA32 for upload to avoid rendering artifacts.
    cv::Mat upload_mat;
    if (channels == 3) {
        cv::cvtColor(mat, upload_mat, cv::COLOR_RGB2RGBA);
        channels = 4;
    } else if (channels == 4) {
        upload_mat = mat;
    } else if (channels == 1) {
        cv::cvtColor(mat, upload_mat, cv::COLOR_GRAY2RGBA);
        channels = 4;
    } else {
        return;
    }

    Uint32 sdl_format = SDL_PIXELFORMAT_RGBA32;

    if (entry.texture) {
        SDL_DestroyTexture(entry.texture);
        entry.texture = nullptr;
    }

    entry.texture = SDL_CreateTexture(state_->renderer, sdl_format,
                                       SDL_TEXTUREACCESS_STREAMING,
                                       w, h);
    if (!entry.texture) return;

    void* pixels = nullptr;
    int pitch = 0;
    if (SDL_LockTexture(entry.texture, nullptr, &pixels, &pitch) == 0) {
        size_t src_row_bytes = static_cast<size_t>(w * channels);
        size_t dst_pitch = static_cast<size_t>(pitch);

        if (dst_pitch == src_row_bytes && upload_mat.isContinuous()) {
            std::memcpy(pixels, upload_mat.ptr(), static_cast<size_t>(h) * src_row_bytes);
        } else {
            for (int y = 0; y < h; y++) {
                std::memcpy(static_cast<uint8_t*>(pixels) + y * pitch,
                           upload_mat.ptr(y), src_row_bytes);
            }
        }
        SDL_UnlockTexture(entry.texture);
    }

    entry.tex_w = w;
    entry.tex_h = h;
    entry.texture_dirty = false;
}

void App::update_diff_texture() {
    if (!diff_texture_.dirty) return;
    diff_texture_.dirty = false;

    diff_image_.reset();

    if (selected_.size() != 2) return;

    int idx_a = -1, idx_b = -1;
    get_ab_indices(idx_a, idx_b);
    if (idx_a < 0 || idx_b < 0) return;

    if (idx_a >= static_cast<int>(entries_.size())) return;
    if (idx_b >= static_cast<int>(entries_.size())) return;

    const auto* img_a = entries_[idx_a].display_image
                            ? entries_[idx_a].display_image.get()
                            : entries_[idx_a].image.get();
    const auto* img_b = entries_[idx_b].display_image
                            ? entries_[idx_b].display_image.get()
                            : entries_[idx_b].image.get();

    if (!img_a || !img_b) return;

    ImageComparator comparator;
    DifferenceOptions opts;
    opts.amplification = 5.0;
    opts.heatmap_color = HeatmapColor::Inferno;

    auto diff = comparator.compute_difference(*img_a, *img_b, opts);
    if (!diff) {
        state_->status_text = "Diff: " + comparator.last_error();
        return;
    }

    auto heatmap = comparator.compute_heatmap(*diff, opts);
    if (!heatmap) {
        state_->status_text = "Heatmap: " + comparator.last_error();
        return;
    }

    diff_image_ = std::move(heatmap);
    upload_diff_texture();
}

void App::upload_diff_texture() {
    if (!diff_image_) return;

    const auto& mat = diff_image_->mat();
    if (mat.empty()) return;

    int w = mat.cols;
    int h = mat.rows;
    int channels = mat.channels();

    // Heatmap is in RGB order (converted in image_comparator).
    // Convert to RGBA32 for SDL texture upload.
    cv::Mat upload_mat;
    if (channels == 4) {
        upload_mat = mat;
    } else if (channels == 3) {
        cv::cvtColor(mat, upload_mat, cv::COLOR_RGB2RGBA);
    } else if (channels == 1) {
        cv::cvtColor(mat, upload_mat, cv::COLOR_GRAY2RGBA);
    } else {
        return;
    }
    channels = 4;

    Uint32 sdl_format = SDL_PIXELFORMAT_RGBA32;

    if (diff_texture_.texture) {
        SDL_DestroyTexture(diff_texture_.texture);
    }

    diff_texture_.texture = SDL_CreateTexture(state_->renderer, sdl_format,
                                               SDL_TEXTUREACCESS_STREAMING,
                                               w, h);
    if (!diff_texture_.texture) return;

    void* pixels = nullptr;
    int pitch = 0;
    if (SDL_LockTexture(diff_texture_.texture, nullptr, &pixels, &pitch) == 0) {
        size_t src_row_bytes = static_cast<size_t>(w * channels);
        size_t dst_pitch = static_cast<size_t>(pitch);

        if (dst_pitch == src_row_bytes && upload_mat.isContinuous()) {
            std::memcpy(pixels, upload_mat.ptr(), static_cast<size_t>(h) * src_row_bytes);
        } else {
            for (int y = 0; y < h; y++) {
                std::memcpy(static_cast<uint8_t*>(pixels) + y * pitch,
                           upload_mat.ptr(y), src_row_bytes);
            }
        }
        SDL_UnlockTexture(diff_texture_.texture);
    }

    diff_texture_.tex_w = w;
    diff_texture_.tex_h = h;
}

void App::render_toolbar() {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open Images...", "Ctrl+O")) {
                open_file_dialog();
            }
            if (ImGui::MenuItem("Save Viewport As...", "Ctrl+S",
                                 false,
                                 !entries_.empty())) {
                save_viewport_dialog();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Quit", "Alt+F4")) {
                SDL_Event e;
                e.type = SDL_QUIT;
                SDL_PushEvent(&e);
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Image List", nullptr, &state_->show_image_list);
            ImGui::MenuItem("Metrics", nullptr, &state_->show_metrics);
            ImGui::MenuItem("Properties", nullptr, &state_->show_properties);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Upscale")) {
            bool is_nearest = state_->upscale_method == UpscaleMethod::Nearest;
            bool is_bilinear = state_->upscale_method == UpscaleMethod::Bilinear;
            bool is_bicubic = state_->upscale_method == UpscaleMethod::Bicubic;
            bool is_lanczos = state_->upscale_method == UpscaleMethod::Lanczos;

            if (ImGui::Checkbox("Nearest", &is_nearest) && is_nearest) {
                state_->upscale_method = UpscaleMethod::Nearest;
            }
            if (ImGui::Checkbox("Bilinear", &is_bilinear) && is_bilinear) {
                state_->upscale_method = UpscaleMethod::Bilinear;
            }
            if (ImGui::Checkbox("Bicubic", &is_bicubic) && is_bicubic) {
                state_->upscale_method = UpscaleMethod::Bicubic;
            }
            if (ImGui::Checkbox("Lanczos", &is_lanczos) && is_lanczos) {
                state_->upscale_method = UpscaleMethod::Lanczos;
            }

            ImGui::EndMenu();
        }

        ImGui::Separator();
        if (ImGui::SmallButton("+ Open")) {
            open_file_dialog();
        }

        ImGui::EndMainMenuBar();
    }
}

void App::render_image_list() {
    ImGui::SetNextWindowSize(ImVec2(220, 400), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Images", &state_->show_image_list)) {
        ImGui::End();
        return;
    }

    if (ImGui::Button("+ Add Images", ImVec2(-1, 0))) {
        open_file_dialog();
    }

    if (selected_.size() >= 2) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear")) {
            selected_.clear();
            swap_ab_ = false;
            diff_texture_.dirty = true;
        }
    }

    ImGui::Separator();

    if (!selected_.empty()) {
        ImGui::TextColored(ImVec4(0.40f, 0.80f, 1.00f, 1.00f),
                           "%zu selected", selected_.size());
    }

    float list_height = ImGui::GetContentRegionAvail().y;
    if (ImGui::BeginChild("##image_list_child", ImVec2(0, list_height), false)) {
        // The first two selected entries drive overlay / diff. A/B may be
        // swapped by the user via the viewport's Swap A/B button.
        int ab_idx[2] = {-1, -1};
        get_ab_indices(ab_idx[0], ab_idx[1]);

        for (int i = 0; i < static_cast<int>(entries_.size()); i++) {
            auto& entry = entries_[i];
            ImGui::PushID(i);

            bool is_sel = selected_.count(i) > 0;
            const char* ab_tag = (i == ab_idx[0]) ? "A"
                                : (i == ab_idx[1]) ? "B" : nullptr;

            if (is_sel) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.40f, 0.80f, 1.00f, 1.00f));
            }

            bool checked = is_sel;
            if (ImGui::Checkbox("##sel", &checked)) {
                if (checked) {
                    selected_.insert(i);
                } else {
                    selected_.erase(i);
                }
                // Selection membership changed -- reset A/B swap so the
                // user's intent isn't attached to stale slot mapping.
                swap_ab_ = false;
                // Selection change affects upscale targets for all selected images
                for (int s : selected_) {
                    if (s >= 0 && s < static_cast<int>(entries_.size())) {
                        entries_[s].texture_dirty = true;
                    }
                }
                diff_texture_.dirty = true;
            }

            ImGui::SameLine();

            // Draw A / B tag as a pill in front of the label so the user
            // can tell which selected entries feed overlay / diff.
            if (ab_tag) {
                ImVec4 pill_col = (ab_tag[0] == 'A')
                    ? ImVec4(0.30f, 0.70f, 1.00f, 1.00f)    // A: blue
                    : ImVec4(1.00f, 0.55f, 0.25f, 1.00f);   // B: orange
                ImGui::PushStyleColor(ImGuiCol_Button, pill_col);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, pill_col);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, pill_col);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
                ImGui::SmallButton(ab_tag);
                ImGui::PopStyleColor(4);
                ImGui::SameLine();
            }

            // Selectable for the label — also serves as drag source/target
            ImGui::Selectable(entry.display_label.c_str(), is_sel,
                              ImGuiSelectableFlags_AllowOverlap);

            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", entry.path.c_str());
            }

            // Drag source
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                drag_source_idx_ = i;
                ImGui::SetDragDropPayload("IMAGE_REORDER", &i, sizeof(int));
                ImGui::Text("%s", entry.display_label.c_str());
                ImGui::EndDragDropSource();
            }

            // Drop target
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("IMAGE_REORDER")) {
                    int src = *static_cast<const int*>(payload->Data);
                    move_entry(src, i);
                    drag_source_idx_ = -1;
                    drag_target_idx_ = -1;
                }
                ImGui::EndDragDropTarget();
            }

            if (ImGui::BeginPopupContextItem("entry_ctx")) {
                if (ImGui::MenuItem("Remove")) { remove_entry(i); }
                ImGui::EndPopup();
            }

            if (is_sel) {
                ImGui::PopStyleColor();
            }

            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    if (entries_.empty()) {
        ImGui::TextDisabled("No images loaded.");
        ImGui::TextDisabled("Ctrl+O or click '+' to add.");
    }

    ImGui::End();
}

void App::render_viewport() {
    // Upload dirty textures for selected images
    for (int s : selected_) {
        if (s >= 0 && s < static_cast<int>(entries_.size())) {
            if (entries_[s].texture_dirty) {
                update_display_image(s);
                upload_texture(entries_[s]);
            }
        }
    }

    update_diff_texture();

    // Build texture list from selected images. Place A then B in the first
    // two slots (honoring the swap flag), followed by any additional
    // selected images in their natural order.
    std::vector<SDL_Texture*> tex_ptrs;
    std::vector<int> tex_ws, tex_hs;
    std::vector<const char*> labels;
    // Hold label storage so const char* remains valid for the frame
    std::vector<std::string> label_storage;
    label_storage.reserve(selected_.size());

    // Reset the slot->entry mapping; repopulated below in lockstep with the
    // vectors above so render_status_bar can map hovered slots back.
    viewport_slot_to_entry_.clear();
    viewport_slot_to_entry_.reserve(selected_.size());

    int ab_idx[2] = {-1, -1};
    get_ab_indices(ab_idx[0], ab_idx[1]);

    auto push_entry = [&](int s, const char* prefix) {
        if (s < 0 || s >= static_cast<int>(entries_.size())) return;
        tex_ptrs.push_back(entries_[s].texture);
        tex_ws.push_back(entries_[s].tex_w);
        tex_hs.push_back(entries_[s].tex_h);
        std::string lbl = prefix
            ? (std::string("[") + prefix + "] " + entries_[s].display_label)
            : entries_[s].display_label;
        label_storage.push_back(std::move(lbl));
        labels.push_back(label_storage.back().c_str());
        viewport_slot_to_entry_.push_back(s);
    };

    if (ab_idx[0] >= 0) push_entry(ab_idx[0], "A");
    if (ab_idx[1] >= 0) push_entry(ab_idx[1], "B");
    for (int s : selected_) {
        if (s == ab_idx[0] || s == ab_idx[1]) continue;
        push_entry(s, nullptr);
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
    if (!ImGui::Begin("Viewport", nullptr,
                       ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }

    auto& vp = *state_->viewport;
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
    if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
        ImVec2 delta = io.MouseDelta;
        vp.set_pan(vp.pan_x() + delta.x, vp.pan_y() + delta.y);
    }

    // --- Right-mouse drag for selection rectangle zoom ---
    if (hovered || vp.selecting()) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && hovered) {
            vp.begin_selection(io.MousePos);
        }
        if (vp.selecting()) {
            vp.update_selection(io.MousePos);
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
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
        // '0' or Ctrl+0 : fit to content
        bool ctrl = io.KeyCtrl;
        if (ImGui::IsKeyPressed(ImGuiKey_0) || ImGui::IsKeyPressed(ImGuiKey_Keypad0)) {
            vp.fit_to_content();
        }
        // '1' or Ctrl+1 : actual pixels (100%)
        if (ImGui::IsKeyPressed(ImGuiKey_1) || ImGui::IsKeyPressed(ImGuiKey_Keypad1)) {
            vp.zoom_to_actual();
        }
        // 'F' : fit to content
        if (ImGui::IsKeyPressed(ImGuiKey_F) && !ctrl) {
            vp.fit_to_content();
        }
    }

    // --- Double-click to fit ---
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
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
        vp.set_mode(static_cast<ComparisonMode>(mode_int));

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
        bool can_save = !entries_.empty() &&
                        (!selected_.empty() ||
                         (vp.mode() == ComparisonMode::Difference && diff_image_));
        ImGui::BeginDisabled(!can_save);
        if (ImGui::SmallButton("Save...")) {
            save_viewport_dialog();
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Save the current viewport (Split / Overlay / Diff) "
                              "to a PNG or JPEG file");
        }

        int sel_count = static_cast<int>(selected_.size());
        if (sel_count >= 2) {
            ImGui::SameLine();
            ImGui::Spacing();
            ImGui::SameLine();
            if (ImGui::SmallButton("Swap A/B")) {
                swap_ab_ = !swap_ab_;
                diff_texture_.dirty = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Swap which selected image acts as A and B");
            }
        }

        if (sel_count > 0) {
            ImGui::SameLine();
            ImGui::Spacing();
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.40f, 0.80f, 1.00f, 1.00f),
                               "%d images", sel_count);
        }

        ImGui::Separator();
    }

    // Render viewport content
    vp.render(tex_ptrs, tex_ws, tex_hs, labels,
              diff_texture_.texture, diff_texture_.tex_w, diff_texture_.tex_h);

    ImGui::End();
    ImGui::PopStyleVar();
}

void App::render_right_sidebar() {
    ImGui::SetNextWindowSize(ImVec2(300, 400), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Inspector")) {
        ImGui::End();
        return;
    }

    // Resolve A and B via the shared helper so the inspector matches the
    // viewport / image list labeling (including the Swap A/B toggle).
    int ab_idx[2] = {-1, -1};
    get_ab_indices(ab_idx[0], ab_idx[1]);

    auto get_entry = [&](int idx) -> const ImageEntry* {
        if (idx < 0 || idx >= static_cast<int>(entries_.size())) return nullptr;
        return &entries_[idx];
    };
    const ImageEntry* entry_a = get_entry(ab_idx[0]);
    const ImageEntry* entry_b = get_entry(ab_idx[1]);

    const Image* img_a = entry_a ? entry_a->image.get() : nullptr;
    const Image* img_b = entry_b ? entry_b->image.get() : nullptr;
    const Image* disp_a = entry_a ? (entry_a->display_image ? entry_a->display_image.get()
                                                            : entry_a->image.get())
                                  : nullptr;
    const Image* disp_b = entry_b ? (entry_b->display_image ? entry_b->display_image.get()
                                                            : entry_b->image.get())
                                  : nullptr;
    const char* name_a = entry_a ? entry_a->display_label.c_str() : nullptr;
    const char* name_b = entry_b ? entry_b->display_label.c_str() : nullptr;

    if (ImGui::BeginTabBar("##inspector_tabs")) {
        if (ImGui::BeginTabItem("Properties")) {
            if (state_->properties_panel) {
                state_->properties_panel->render_inline(img_a, img_b, disp_a, disp_b,
                                                        name_a, name_b);
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Metrics")) {
            if (state_->metrics_panel) {
                // Metrics always compare A vs B in the same order as the
                // inspector / viewport, so the swap flag propagates here too.
                state_->metrics_panel->render_inline(disp_a, disp_b);
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}

void App::render_status_bar() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + vp->Size.y - 24));
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x, 24));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 4));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar |
                              ImGuiWindowFlags_NoSavedSettings |
                              ImGuiWindowFlags_MenuBar;

    if (ImGui::Begin("##statusbar", nullptr, flags)) {
        if (ImGui::BeginMenuBar()) {
            char buf[512];
            int n = std::snprintf(buf, sizeof(buf), "%zu images | %zu selected",
                                   entries_.size(), selected_.size());
            auto append = [&](const char* fmt, ...) {
                if (n < 0 || static_cast<size_t>(n) >= sizeof(buf)) return;
                va_list ap;
                va_start(ap, fmt);
                int m = std::vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
                va_end(ap);
                if (m > 0) n += m;
            };

            int ab_idx[2] = {-1, -1};
            get_ab_indices(ab_idx[0], ab_idx[1]);
            if (ab_idx[0] >= 0 && ab_idx[0] < static_cast<int>(entries_.size())) {
                append(" | A: %s", entries_[ab_idx[0]].display_label.c_str());
            }
            if (ab_idx[1] >= 0 && ab_idx[1] < static_cast<int>(entries_.size())) {
                append(" | B: %s", entries_[ab_idx[1]].display_label.c_str());
            }
            int extra = static_cast<int>(selected_.size()) -
                        ((ab_idx[0] >= 0 ? 1 : 0) + (ab_idx[1] >= 0 ? 1 : 0));
            if (extra > 0) {
                append(" (+%d shown, ignored by overlay/diff)", extra);
            }

            // Hover pixel readout — resolved against the display image that
            // the viewport actually drew (so coordinates match what's on
            // screen, even when one image was upscaled to match the other).
            auto& vport = *state_->viewport;
            if (vport.hover_valid()) {
                int cell = vport.hover_cell_index();
                int px = vport.hover_pixel_x();
                int py = vport.hover_pixel_y();

                const char* src_label = nullptr;
                const Image* src_img = nullptr;

                if (vport.mode() == ComparisonMode::Difference) {
                    src_label = "Diff";
                    src_img = diff_image_.get();
                } else if (cell >= 0 &&
                           cell < static_cast<int>(viewport_slot_to_entry_.size())) {
                    int ent = viewport_slot_to_entry_[cell];
                    if (ent >= 0 && ent < static_cast<int>(entries_.size())) {
                        const auto& e = entries_[ent];
                        // Prefer display_image because pixel coords from the
                        // viewport correspond to the image that was rendered.
                        src_img = e.display_image ? e.display_image.get()
                                                  : e.image.get();
                        src_label = e.display_label.c_str();
                    }
                }

                append(" | %s @ (%d, %d)", src_label ? src_label : "?", px, py);

                if (src_img) {
                    const auto& m = src_img->mat();
                    if (!m.empty() &&
                        px >= 0 && px < m.cols && py >= 0 && py < m.rows) {
                        int ch = m.channels();
                        int depth = m.depth();  // CV_8U = 0, CV_16U = 2
                        if (depth == 0) {  // 8-bit
                            const uint8_t* p = m.ptr<uint8_t>(py) + px * ch;
                            if (ch == 1)      append(" = %u", p[0]);
                            else if (ch == 3) append(" = (%u, %u, %u)",
                                                     p[0], p[1], p[2]);
                            else if (ch == 4) append(" = (%u, %u, %u, %u)",
                                                     p[0], p[1], p[2], p[3]);
                        } else if (depth == 2) {  // 16-bit
                            const uint16_t* p = m.ptr<uint16_t>(py) + px * ch;
                            if (ch == 1)      append(" = %u", p[0]);
                            else if (ch == 3) append(" = (%u, %u, %u)",
                                                     p[0], p[1], p[2]);
                            else if (ch == 4) append(" = (%u, %u, %u, %u)",
                                                     p[0], p[1], p[2], p[3]);
                        }
                    }
                }
            }

            if (!state_->status_text.empty()) {
                append(" | %s", state_->status_text.c_str());
            }
            ImGui::TextUnformatted(buf);
            ImGui::EndMenuBar();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

} // namespace idiff
