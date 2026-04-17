#include "app/app.h"
#include "app/viewport.h"
#include "app/metrics_panel.h"
#include "app/properties_panel.h"
#include "app/settings.h"

#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>
#include <imgui_internal.h>
#include <SDL.h>
#include <nfd.h>

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <numeric>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "core/image_loader.h"
#include "core/image_processor.h"
#include "core/image_comparator.h"
#include "core/media_source.h"
#include "core/comparison_config.h"
#include "core/url_cache.h"

namespace idiff {

struct App::State {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;

    std::unique_ptr<Viewport> viewport;
    std::unique_ptr<MetricsPanel> metrics_panel;
    std::unique_ptr<PropertiesPanel> properties_panel;

    UpscaleMethod upscale_method = UpscaleMethod::Lanczos;

    // Which image loader backend load_images() should prefer.  ImageMagick
    // is chosen by default when compiled in because it handles ICC profiles
    // and a wider set of formats; users can switch to OpenCV via the View
    // menu to observe decoding differences.
    LoaderBackend loader_backend = ImageLoader::default_backend();

    std::string status_text;
    bool show_metrics = true;
    bool show_properties = true;
    bool show_image_list = true;
    int sidebar_tab = 0;

    // Persistent cross-session settings (currently just last-used YUV
    // parameters).  Loaded in App::init(), saved whenever a YUV file is
    // successfully added.
    AppSettings settings;

    // YUV-parameters dialog state.  When pending_yuv_paths is non-empty,
    // frame() opens a modal for the front element; the user either
    // confirms (turning it into a YuvRawSource entry) or skips it.
    std::vector<std::string> pending_yuv_paths;
    YuvStreamParams yuv_dialog_params{};
    // When true, the dialog was just primed with a new path and must call
    // ImGui::OpenPopup on the next render.  Gets cleared after opening.
    bool yuv_dialog_needs_open = false;
    // When >= 0, the YUV dialog is in "edit" mode targeting entries_[idx]
    // rather than loading a new file from pending_yuv_paths.  Reset to -1
    // once the edit is confirmed or cancelled.
    int editing_yuv_entry_idx = -1;

    // Shared timeline index.  All multi-frame entries display
    // frame(current_frame + entry.frame_offset), clamped to each entry's
    // own frame_count.  Single-frame entries ignore this value.  The
    // timeline bar exposes this as a slider.
    int current_frame = 0;

    // Comparison-config support.  When the user opens a JSON config we
    // keep the parsed groups here and load them on demand.  Only one
    // group at a time is loaded into `entries_` so memory stays bounded
    // regardless of how many groups the document lists.
    ComparisonConfig comparison_config;
    int current_group_idx = -1;  // -1 = no config loaded
    // Download cache, scoped to the currently-loaded config.  Each
    // config gets its own subdirectory (idiff_cache_<stem>_<timestamp>)
    // under the configured cache root / Downloads folder, so the user
    // can later tell which cache dump came from which comparison JSON.
    // Reset to nullptr when no config is loaded.
    std::unique_ptr<UrlCache> url_cache;
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

    // Load persistent settings (last-used YUV params, etc.).  A missing
    // file is fine -- AppSettings::load falls back to defaults.
    state_->settings = AppSettings::load();
    // Seed the dialog with whatever the user last confirmed so they do
    // not have to retype resolution / pixel-format for each file.
    state_->yuv_dialog_params = state_->settings.last_yuv_params;

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

    // Global keyboard shortcuts.  Only fire when no text input widget is
    // capturing keystrokes so typing in e.g. search boxes won't trigger a
    // file dialog.  Shortcuts mirror what's advertised in the File menu.
    {
        ImGuiIO& io = ImGui::GetIO();
        if (!io.WantTextInput) {
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_O)) {
                open_file_dialog();
            }
            if (!entries_.empty() &&
                ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) {
                save_viewport_dialog();
            }
        }
    }

    render_toolbar();

    ImGuiViewport* vp = ImGui::GetMainViewport();

    // Reserve a strip at the bottom of the work area for the status bar.
    // The DockSpace host window is shrunk by this amount so docked panels
    // (Images / Viewport / Inspector) don't overlap and hide the status bar.
    // render_status_bar() uses the same height to position itself.
    float status_bar_h = ImGui::GetFrameHeightWithSpacing();

    // If any loaded source exposes more than one frame we also reserve a
    // variable-height strip above the status bar for the timeline slider
    // and per-entry offsets.  Computed up front so the docking area and
    // the timeline / status bar agree on layout.
    float timeline_h = 0.0f;
    if (timeline_length() > 1) {
        int offset_rows = 0;
        for (const auto& e : entries_) {
            if (e.source && e.source->frame_count() > 1) offset_rows++;
        }
        int visible_offset_rows = std::min(offset_rows, 4);
        timeline_h = ImGui::GetFrameHeightWithSpacing()
                   * (1.0f + visible_offset_rows) + 8.0f;
    }

    ImVec2 dock_pos = vp->WorkPos;
    ImVec2 dock_size = vp->WorkSize;
    dock_size.y = std::max(0.0f,
                          dock_size.y - status_bar_h - timeline_h);

    ImGui::SetNextWindowPos(dock_pos);
    ImGui::SetNextWindowSize(dock_size);
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
    render_timeline_bar();
    render_status_bar();
    render_yuv_params_dialog();

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
    // Detect the "first load" case — the list was empty before this call.
    // We only auto-select / auto-switch mode in that scenario so that later
    // "append more images" calls do not clobber the user's current picks or
    // comparison mode.
    const bool was_empty = entries_.empty();

    for (const auto& path : paths) {
        // Raw YUV files carry no decoding metadata, so we cannot load
        // them synchronously here.  Queue them for the parameter dialog
        // which runs during the next frame and creates YuvRawSource
        // entries on confirmation.
        auto dot = path.find_last_of('.');
        std::string ext;
        if (dot != std::string::npos) ext = path.substr(dot);
        std::string ext_lower = ext;
        for (auto& c : ext_lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext_lower == ".yuv") {
            state_->pending_yuv_paths.push_back(path);
            state_->yuv_dialog_needs_open = true;
            continue;
        }

        // Wrap the file in an ImageFileSource so every entry looks like a
        // (single-frame) MediaSource to the rest of the app.  Decoding
        // happens inside source->read_frame(0), which internally uses the
        // same ImageLoader pipeline as before.
        auto source = std::make_unique<ImageFileSource>(path, state_->loader_backend);
        auto img = source->read_frame(0);
        if (img) {
            ImageEntry entry;
            entry.path = path;
            auto sep = path.find_last_of("/\\");
            entry.filename = (sep != std::string::npos) ? path.substr(sep + 1) : path;
            entry.display_label = entry.filename;
            entry.source = std::move(source);
            entry.image = std::move(img);
            entry.display_image = nullptr;
            entry.texture = nullptr;
            entry.texture_dirty = true;

            entries_.push_back(std::move(entry));
            state_->status_text = "Loaded: " + path;
        } else {
            state_->status_text = "Failed to load: " + path + " (" + source->last_error() + ")";
        }
    }

    sort_entries_by_name();
    compute_display_labels();
    diff_texture_.dirty = true;

    // Convenience: on the first successful load, auto-select up to the first
    // two images and switch to Overlay mode.  This removes the need for the
    // user to manually tick two checkboxes before seeing any comparison, and
    // matches the most common use case (drop two images in, compare them).
    if (was_empty && !entries_.empty()) {
        selected_.clear();
        swap_ab_ = false;
        int pick = std::min<int>(2, static_cast<int>(entries_.size()));
        for (int i = 0; i < pick; i++) selected_.insert(i);
        // Flag newly-selected entries for texture/display refresh so the
        // viewport renders them immediately on the next frame.
        for (int s : selected_) {
            if (s >= 0 && s < static_cast<int>(entries_.size())) {
                entries_[s].texture_dirty = true;
            }
        }
        diff_texture_.dirty = true;
        if (state_->viewport) {
            state_->viewport->set_mode(ComparisonMode::Overlay);
        }
    }
}

// Rerun the image loader over every entry using the currently-selected
// backend.  This is triggered when the user changes the "Image Loader"
// choice in the View menu so they can eyeball decoding differences (ICC
// handling, bit-depth, exotic formats) between ImageMagick and OpenCV.
// Entries whose re-load fails are kept with their previous pixel data so
// the viewport does not suddenly go blank; a status message tells the
// user which file failed.
void App::reload_all_images() {
    if (entries_.empty()) return;

    int reloaded = 0;
    int failed = 0;
    std::string last_fail;
    for (auto& entry : entries_) {
        // Update the backend preference on the source, then ask it to
        // re-decode the current frame.  For video sources this will be
        // tracked by the shared frame index once time-axis wiring lands;
        // for still images the index is always 0.
        if (auto* ifs = dynamic_cast<ImageFileSource*>(entry.source.get())) {
            ifs->set_preferred_backend(state_->loader_backend);
        }
        auto img = entry.source ? entry.source->read_frame(0) : nullptr;
        if (img) {
            entry.image = std::move(img);
            entry.display_image.reset();
            entry.texture_dirty = true;
            reloaded++;
        } else {
            failed++;
            std::string err = entry.source ? entry.source->last_error() : "no source";
            last_fail = entry.filename + " (" + err + ")";
        }
    }
    diff_texture_.dirty = true;

    const char* name = ImageLoader::backend_name(state_->loader_backend);
    if (failed == 0) {
        state_->status_text = std::string("Reloaded ")
            + std::to_string(reloaded) + " image(s) via " + name;
    } else {
        state_->status_text = std::string("Reloaded ")
            + std::to_string(reloaded) + " via " + name + ", "
            + std::to_string(failed) + " failed: " + last_fail;
    }
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

bool App::add_yuv_entry(const std::string& path, const YuvStreamParams& params) {
    auto source = std::make_unique<YuvRawSource>(path, params);
    if (source->frame_count() <= 0) {
        state_->status_text = "YUV: invalid parameters or unreadable file: " + path;
        return false;
    }
    auto img = source->read_frame(0);
    if (!img) {
        state_->status_text = "YUV: decode failed for " + path +
                              " (" + source->last_error() + ")";
        return false;
    }

    const bool was_empty = entries_.empty();

    ImageEntry entry;
    entry.path = path;
    auto sep = path.find_last_of("/\\");
    entry.filename = (sep != std::string::npos) ? path.substr(sep + 1) : path;
    // Include frame count in the label so the list shows e.g.
    // "clip.yuv (300 frames)" for multi-frame streams.
    entry.display_label = entry.filename;
    if (source->frame_count() > 1) {
        entry.display_label += " (" + std::to_string(source->frame_count())
                            + " frames)";
    }
    entry.source = std::move(source);
    entry.image = std::move(img);
    entry.display_image = nullptr;
    entry.texture = nullptr;
    entry.texture_dirty = true;

    entries_.push_back(std::move(entry));

    sort_entries_by_name();
    compute_display_labels();
    diff_texture_.dirty = true;

    // Persist parameters so the next .yuv file starts with the same
    // defaults in the dialog.
    state_->settings.last_yuv_params = params;
    state_->settings.save();

    // Mirror the "first load" convenience from load_images(): if the
    // entry list was empty before this add, auto-select up to the first
    // two items and switch to Overlay.
    if (was_empty && !entries_.empty()) {
        selected_.clear();
        swap_ab_ = false;
        int pick = std::min<int>(2, static_cast<int>(entries_.size()));
        for (int i = 0; i < pick; i++) selected_.insert(i);
        for (int s : selected_) {
            if (s >= 0 && s < static_cast<int>(entries_.size())) {
                entries_[s].texture_dirty = true;
            }
        }
        diff_texture_.dirty = true;
        if (state_->viewport) {
            state_->viewport->set_mode(ComparisonMode::Overlay);
        }
    }

    state_->status_text = "Loaded YUV: " + path;
    return true;
}

void App::begin_edit_yuv_entry(int index) {
    if (index < 0 || index >= static_cast<int>(entries_.size())) return;
    auto* yuv = dynamic_cast<YuvRawSource*>(entries_[index].source.get());
    if (!yuv) return;  // not a YUV stream; silently ignore

    // Seed the dialog with this entry's actual current parameters so the
    // user tweaks from the existing configuration rather than from
    // settings defaults or last_yuv_params.
    state_->yuv_dialog_params = yuv->params();
    state_->editing_yuv_entry_idx = index;
    state_->yuv_dialog_needs_open = true;
}

bool App::update_yuv_entry_params(int index, const YuvStreamParams& params) {
    if (index < 0 || index >= static_cast<int>(entries_.size())) return false;
    auto& entry = entries_[index];

    // Build the new source first; only swap on success so a bad edit
    // leaves the existing (working) stream untouched.
    auto source = std::make_unique<YuvRawSource>(entry.path, params);
    if (source->frame_count() <= 0) {
        state_->status_text = "YUV: invalid parameters for " + entry.path;
        return false;
    }
    // Attempt to keep the current timeline position when possible so the
    // user sees what the fix did on the frame they were inspecting.
    int target_frame = state_->current_frame + entry.frame_offset;
    if (target_frame < 0) target_frame = 0;
    if (target_frame >= source->frame_count()) {
        target_frame = source->frame_count() - 1;
    }
    auto img = source->read_frame(target_frame);
    if (!img) {
        state_->status_text = "YUV: decode failed for " + entry.path +
                              " (" + source->last_error() + ")";
        return false;
    }

    entry.source = std::move(source);
    entry.image = std::move(img);
    entry.display_image.reset();
    entry.texture_dirty = true;
    entry.cached_frame = target_frame;

    // Refresh "(N frames)" suffix: may change if the new params produce a
    // different frame count.  compute_display_labels() will reconcile
    // uniqueness and path stripping for free.
    auto sep = entry.path.find_last_of("/\\");
    entry.filename = (sep != std::string::npos)
                       ? entry.path.substr(sep + 1) : entry.path;
    entry.display_label = entry.filename;
    if (entry.source->frame_count() > 1) {
        entry.display_label += " (" + std::to_string(entry.source->frame_count())
                            + " frames)";
    }
    compute_display_labels();

    diff_texture_.dirty = true;

    // Remember the successful parameters as the new load-dialog default.
    state_->settings.last_yuv_params = params;
    state_->settings.save();

    state_->status_text = "Updated YUV parameters: " + entry.filename;
    return true;
}



void App::render_yuv_params_dialog() {
    const bool editing = state_->editing_yuv_entry_idx >= 0;

    // In load mode the dialog is driven by the pending-paths queue.
    // In edit mode it targets an existing entry; nothing is queued.
    if (!editing && state_->pending_yuv_paths.empty()) return;

    // Resolve the path used for the header / file-size preview.
    std::string current_path;
    if (editing) {
        int idx = state_->editing_yuv_entry_idx;
        if (idx < 0 || idx >= static_cast<int>(entries_.size())) {
            // Entry disappeared (removed, reordered out of range, ...).
            // Abort the edit session silently.
            state_->editing_yuv_entry_idx = -1;
            return;
        }
        current_path = entries_[idx].path;
    } else {
        current_path = state_->pending_yuv_paths.front();
    }

    // First-frame priming: seed dialog params with the appropriate source.
    // Load mode uses the last-confirmed defaults + filename guess; edit
    // mode uses the entry's actual current parameters so the user starts
    // from what the stream is configured with today.
    if (state_->yuv_dialog_needs_open) {
        if (!editing) {
            state_->yuv_dialog_params = state_->settings.last_yuv_params;
            guess_yuv_params_from_filename(current_path, state_->yuv_dialog_params);
        }
        // In edit mode, begin_edit_yuv_entry() has already set
        // yuv_dialog_params from the existing source; don't overwrite it.
        ImGui::OpenPopup("YUV Parameters");
        state_->yuv_dialog_needs_open = false;
    }

    // Center the modal over the main viewport.
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("YUV Parameters", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(editing
            ? "Edit decoder parameters for:"
            : "Configure decoder parameters for:");
        ImGui::TextDisabled("%s", current_path.c_str());
        ImGui::Separator();

        auto& params = state_->yuv_dialog_params;

        ImGui::InputInt("Width",  &params.width);
        ImGui::InputInt("Height", &params.height);

        const char* fmt_items[] = { "YUV420P (I420)", "YUV422P", "YUV444P" };
        int fmt_idx = static_cast<int>(params.pixel_format);
        if (ImGui::Combo("Pixel format", &fmt_idx, fmt_items,
                         IM_ARRAYSIZE(fmt_items))) {
            params.pixel_format = static_cast<YuvPixelFormat>(fmt_idx);
        }

        const char* range_items[] = { "Limited (TV, 16-235)", "Full (PC, 0-255)" };
        int range_idx = static_cast<int>(params.color_range);
        if (ImGui::Combo("Color range", &range_idx, range_items,
                         IM_ARRAYSIZE(range_items))) {
            params.color_range = static_cast<YuvColorRange>(range_idx);
        }

        // Preview the frame size / frame count.  Guards against div-by-zero
        // and provides early feedback when the user types a bad resolution.
        std::size_t frame_bytes = yuv_frame_size_bytes(params);
        if (frame_bytes == 0) {
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1),
                "Invalid: width/height must be positive (and even for 4:2:0/4:2:2)");
        } else {
            std::error_code ec;
            auto fsize = std::filesystem::file_size(current_path, ec);
            if (ec) {
                ImGui::TextDisabled("Frame size: %zu bytes",
                                     static_cast<size_t>(frame_bytes));
            } else {
                int fc = static_cast<int>(fsize / frame_bytes);
                ImGui::Text("Frame size: %zu bytes  |  File has %d frame(s)",
                            static_cast<size_t>(frame_bytes), fc);
                if (fsize % frame_bytes != 0) {
                    ImGui::TextColored(ImVec4(1, 0.7f, 0.2f, 1),
                        "Warning: file size is not an exact multiple of frame size");
                }
            }
        }

        ImGui::Separator();

        bool confirmed = false;
        bool skipped   = false;
        bool cancelled = false;
        const char* confirm_label = editing ? "Apply" : "Load";
        if (ImGui::Button(confirm_label, ImVec2(100, 0))) {
            confirmed = true;
        }
        ImGui::SameLine();
        if (editing) {
            if (ImGui::Button("Cancel", ImVec2(100, 0))) {
                cancelled = true;
            }
        } else {
            if (ImGui::Button("Skip", ImVec2(100, 0))) {
                skipped = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel all", ImVec2(100, 0))) {
                state_->pending_yuv_paths.clear();
                ImGui::CloseCurrentPopup();
            }
        }

        if (confirmed) {
            if (editing) {
                update_yuv_entry_params(state_->editing_yuv_entry_idx, params);
                state_->editing_yuv_entry_idx = -1;
            } else {
                add_yuv_entry(current_path, params);
                state_->pending_yuv_paths.erase(state_->pending_yuv_paths.begin());
                // If there's another file in the queue, arm the dialog for it.
                if (!state_->pending_yuv_paths.empty()) {
                    state_->yuv_dialog_needs_open = true;
                }
            }
            ImGui::CloseCurrentPopup();
        } else if (skipped) {
            state_->pending_yuv_paths.erase(state_->pending_yuv_paths.begin());
            ImGui::CloseCurrentPopup();
            if (!state_->pending_yuv_paths.empty()) {
                state_->yuv_dialog_needs_open = true;
            }
        } else if (cancelled) {
            state_->editing_yuv_entry_idx = -1;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

int App::timeline_length() const {
    int max_frames = 1;
    for (const auto& e : entries_) {
        if (e.source) {
            max_frames = std::max(max_frames, e.source->frame_count());
        }
    }
    return max_frames;
}

void App::sync_entries_to_timeline() {
    bool any_changed = false;
    for (auto& e : entries_) {
        if (!e.source) continue;
        const int count = e.source->frame_count();
        if (count <= 1) continue;  // still image -- timeline doesn't apply

        // Clamp the effective frame index into the entry's own range so
        // offsets that would run past either end simply pin to the edge.
        int target = state_->current_frame + e.frame_offset;
        if (target < 0) target = 0;
        if (target >= count) target = count - 1;

        if (target == e.cached_frame && e.image) continue;

        auto img = e.source->read_frame(target);
        if (!img) {
            // Leave the previously-decoded frame in place so the viewport
            // doesn't go blank; surface the error to the status bar.
            state_->status_text = "Frame read failed for " + e.filename
                                + " (" + e.source->last_error() + ")";
            continue;
        }
        e.image = std::move(img);
        e.display_image.reset();
        e.texture_dirty = true;
        e.cached_frame = target;
        any_changed = true;
    }
    if (any_changed) {
        diff_texture_.dirty = true;
    }
}

float App::render_timeline_bar() {
    const int length = timeline_length();
    if (length <= 1) return 0.0f;

    // Clamp the shared index once per frame so any external mutation
    // (e.g. adding / removing entries) can't leave it past the new end.
    if (state_->current_frame < 0) state_->current_frame = 0;
    if (state_->current_frame >= length) state_->current_frame = length - 1;

    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float row_h = ImGui::GetFrameHeightWithSpacing();

    // Height: one row for the slider + one row per multi-frame entry
    // (capped to 4 so a large number of streams doesn't consume the
    // whole window -- the scrollable child handles overflow).
    int offset_rows = 0;
    for (const auto& e : entries_) {
        if (e.source && e.source->frame_count() > 1) offset_rows++;
    }
    const int visible_offset_rows = std::min(offset_rows, 4);
    const float bar_h = row_h * (1.0f + visible_offset_rows) + 8.0f;

    // Position directly above the status bar.
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
        // Prev / Next buttons around the slider for precise single-step
        // scrubbing.  The slider uses length-1 as the max so the label
        // reads as a frame index (0..N-1).
        if (ImGui::SmallButton("<")) {
            if (state_->current_frame > 0) {
                state_->current_frame--;
                frame_changed = true;
            }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(">")) {
            if (state_->current_frame < length - 1) {
                state_->current_frame++;
                frame_changed = true;
            }
        }
        ImGui::SameLine();

        int frame = state_->current_frame;
        ImGui::SetNextItemWidth(-200.0f);  // leave room for the label
        if (ImGui::SliderInt("##frame", &frame, 0, length - 1,
                             "Frame %d")) {
            state_->current_frame = frame;
            frame_changed = true;
        }
        ImGui::SameLine();
        ImGui::Text("of %d", length);

        // Per-entry offsets.  Wrap in a scrollable child so many streams
        // don't blow up the status strip.
        if (offset_rows > 0) {
            ImGui::BeginChild("##offsets",
                              ImVec2(0, row_h * visible_offset_rows),
                              false);
            for (std::size_t i = 0; i < entries_.size(); ++i) {
                auto& e = entries_[i];
                if (!e.source || e.source->frame_count() <= 1) continue;

                // Effective frame for display feedback.
                int effective = state_->current_frame + e.frame_offset;
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

    if (frame_changed) {
        sync_entries_to_timeline();
    }

    return bar_h;
}



namespace {

// Split a filename into (stem, extension) using the last '.' as the
// delimiter.  A leading dot (e.g. ".hidden") does not start an extension —
// the whole string is treated as the stem, matching the convention used
// by most shells and file managers.
std::pair<std::string_view, std::string_view>
split_stem_ext(std::string_view name) {
    if (name.size() <= 1) return {name, {}};
    auto dot = name.find_last_of('.');
    if (dot == std::string_view::npos || dot == 0) return {name, {}};
    return {name.substr(0, dot), name.substr(dot + 1)};
}

// Compare filenames by (stem, extension) so that a "base" name like
// "kid.jpg" sorts before its derived siblings ("kid-pisa.jpg",
// "kid-pisa-v0.jpg").  The default std::string operator< puts '-' (0x2D)
// before '.' (0x2E) which is the wrong answer here: the user expects the
// shorter root name to come first.
bool filename_less(const std::string& a, const std::string& b) {
    auto [a_stem, a_ext] = split_stem_ext(a);
    auto [b_stem, b_ext] = split_stem_ext(b);
    if (a_stem != b_stem) return a_stem < b_stem;
    return a_ext < b_ext;
}

} // namespace

void App::sort_entries_by_name() {
    // Build a mapping from old index to entry pointer for selected_ fixup
    std::vector<int> old_indices(entries_.size());
    std::iota(old_indices.begin(), old_indices.end(), 0);

    // Sort entries and track the permutation
    std::sort(old_indices.begin(), old_indices.end(), [&](int a, int b) {
        return filename_less(entries_[a].filename, entries_[b].filename);
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
    // A single "All supported" filter is friendlier than a
    // multi-filter drop-down: the user rarely cares whether something
    // is an image or a config, they just want to point at a file and
    // move on.  load_paths() takes care of routing after the fact.
    nfdfilteritem_t filters[] = {
        { "Images, YUV streams, and comparison configs",
          "png,jpg,jpeg,bmp,tiff,tif,webp,dng,cr2,nef,arw,yuv,json" }
    };
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
        load_paths(paths);
    } else if (result == NFD_ERROR) {
        state_->status_text = "File dialog error: " + std::string(NFD_GetError());
    }
}

void App::open_comparison_config_dialog() {
    nfdfilteritem_t filters[] = {
        { "Comparison config (JSON)", "json" },
    };
    nfdchar_t* out_path = nullptr;
    nfdresult_t result = NFD_OpenDialog(&out_path, filters, 1, nullptr);
    if (result != NFD_OKAY) {
        if (result == NFD_ERROR) {
            state_->status_text = "Config dialog error: " +
                                   std::string(NFD_GetError());
        }
        return;
    }
    std::string path = out_path;
    NFD_FreePath(out_path);
    load_comparison_config_from_path(path);
}

void App::load_comparison_config_from_path(const std::string& path) {
    ComparisonConfig cfg = load_comparison_config(path);
    if (!cfg.error.empty()) {
        state_->status_text = "Config: " + cfg.error;
        return;
    }

    // Drop whatever was previously loaded so the user sees a clean
    // switch.  Per the task brief, we keep at most one group's worth of
    // images resident in memory, so we also release entries from any
    // previously-loaded config.
    for (auto& entry : entries_) {
        if (entry.texture) SDL_DestroyTexture(entry.texture);
    }
    entries_.clear();
    selected_.clear();
    swap_ab_ = false;
    if (diff_texture_.texture) {
        SDL_DestroyTexture(diff_texture_.texture);
        diff_texture_.texture = nullptr;
    }
    diff_image_.reset();
    diff_texture_.dirty = true;

    state_->comparison_config = std::move(cfg);
    state_->current_group_idx = -1;

    // Build a cache directory scoped to this particular config file.
    // The root is resolved fresh on every open so edits to
    // ~/.idiff.config take effect without relaunching the app; the
    // per-config subdirectory name encodes the JSON stem + local
    // timestamp so repeated opens produce distinct, traceable dumps.
    std::filesystem::path cache_root = UrlCache::resolve_default_root();
    std::string sub_dir = UrlCache::make_config_cache_dirname(
        std::filesystem::path(path));
    state_->url_cache = std::make_unique<UrlCache>(cache_root / sub_dir);

    // Feed every URL from every group into the cache up front so it
    // can compute the shared host / path prefix and trim it from the
    // local paths.  This keeps the cache directory as shallow as
    // possible while still guaranteeing distinct local files per URL.
    std::vector<std::string> all_urls;
    for (const auto& g : state_->comparison_config.groups) {
        for (const auto& it : g.items) {
            if (!it.url.empty()) all_urls.push_back(it.url);
        }
    }
    state_->url_cache->register_urls(all_urls);

    int total_items = 0;
    for (const auto& g : state_->comparison_config.groups) {
        total_items += static_cast<int>(g.items.size());
    }
    state_->status_text = "Loaded config: " +
        std::to_string(state_->comparison_config.groups.size()) +
        " group(s), " + std::to_string(total_items) + " image(s). " +
        "Cache: " + state_->url_cache->root().string();

    // Start on the first group so the user sees pixels immediately.
    // switch_to_comparison_group() is responsible for the actual
    // download + load_images() handoff.
    if (!state_->comparison_config.groups.empty()) {
        switch_to_comparison_group(0);
    }
}

void App::load_paths(const std::vector<std::string>& paths) {
    // Split incoming paths by extension so the user can drop a JSON
    // config onto the window (or pick one through the generic "Open
    // Images" dialog) without hunting for a dedicated menu entry.
    std::vector<std::string> image_paths;
    std::vector<std::string> json_paths;
    image_paths.reserve(paths.size());
    for (const auto& p : paths) {
        std::string ext = std::filesystem::path(p).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (ext == ".json") {
            json_paths.push_back(p);
        } else {
            image_paths.push_back(p);
        }
    }

    if (!json_paths.empty()) {
        // A comparison config replaces the whole session, so it is
        // meaningless to honour more than one at a time.  If the user
        // mixed JSON and image paths in the same drop/pick, take the
        // first JSON and surface the rest as a status message instead
        // of silently dropping them.
        load_comparison_config_from_path(json_paths.front());
        if (json_paths.size() > 1 || !image_paths.empty()) {
            state_->status_text +=
                "  (ignored " +
                std::to_string(json_paths.size() - 1 + image_paths.size()) +
                " extra file(s) -- config replaces the session)";
        }
        return;
    }

    if (!image_paths.empty()) {
        load_images(image_paths);
    }
}

void App::switch_to_comparison_group(int group_idx) {
    if (group_idx < 0 ||
        group_idx >= static_cast<int>(state_->comparison_config.groups.size())) {
        return;
    }
    if (group_idx == state_->current_group_idx) return;

    // Release the previous group's images first so we never hold two
    // groups' pixels in memory simultaneously.  This is the main memory
    // lever for configs with many large groups.
    for (auto& entry : entries_) {
        if (entry.texture) SDL_DestroyTexture(entry.texture);
    }
    entries_.clear();
    selected_.clear();
    swap_ab_ = false;
    if (diff_texture_.texture) {
        SDL_DestroyTexture(diff_texture_.texture);
        diff_texture_.texture = nullptr;
    }
    diff_image_.reset();
    diff_texture_.dirty = true;

    const auto& group = state_->comparison_config.groups[group_idx];

    // Fetch each URL to disk (reusing cached entries when present) and
    // collect the local paths.  fetch() streams straight to disk so the
    // app's resident set stays small even when a group contains very
    // large images.
    std::vector<std::string> local_paths;
    local_paths.reserve(group.items.size());
    int failures = 0;
    std::string last_err;
    if (!state_->url_cache) {
        // Should not happen -- open_comparison_config_dialog() always
        // creates the cache before we get here -- but guard anyway so
        // a stray call does not dereference nullptr.
        state_->status_text = "No cache configured for comparison group";
        return;
    }
    for (const auto& item : group.items) {
        auto p = state_->url_cache->fetch(item.url);
        if (p.empty()) {
            failures++;
            last_err = state_->url_cache->last_error();
            continue;
        }
        local_paths.push_back(p.string());
    }

    state_->current_group_idx = group_idx;

    if (!local_paths.empty()) {
        load_images(local_paths);
    }

    // After load_images(), re-label entries with the human-friendly names
    // from the config (item.title, or the URL's basename when no title was
    // provided) instead of the opaque sha256 cache filenames that
    // load_images() inferred from the cached paths.  Match entries back to
    // their original config item by local path so the labelling survives
    // sort_entries_by_name() re-ordering.
    if (!entries_.empty()) {
        // Build (local cache path -> group item) lookup for this group.
        // We only populate it for items we actually fetched successfully;
        // everything else keeps its auto-derived filename.
        std::unordered_map<std::string, const ComparisonItem*> by_path;
        for (const auto& item : group.items) {
            auto p = state_->url_cache->path_for(item.url);
            by_path[p.string()] = &item;
        }
        auto url_basename = [](const std::string& url) -> std::string {
            std::size_t end = url.size();
            if (auto q = url.find_first_of("?#"); q != std::string::npos) {
                end = q;
            }
            std::size_t slash = url.find_last_of('/',
                                    end ? end - 1 : 0);
            std::size_t beg = (slash == std::string::npos) ? 0 : slash + 1;
            if (beg >= end) return {};
            return url.substr(beg, end - beg);
        };
        for (auto& e : entries_) {
            auto it = by_path.find(e.path);
            if (it == by_path.end() || !it->second) continue;
            const auto& item = *it->second;
            std::string label = !item.title.empty()
                                ? item.title : url_basename(item.url);
            if (label.empty()) continue;
            e.filename = label;
            e.display_label = label;
        }
        // compute_display_labels() will uniquify labels if duplicates
        // exist after the rename.
        compute_display_labels();
    }

    std::string msg = "Group \"" + group.name + "\": loaded " +
                       std::to_string(local_paths.size()) + "/" +
                       std::to_string(group.items.size()) + " image(s)";
    if (failures > 0) {
        msg += " (" + std::to_string(failures) + " download failure";
        if (failures > 1) msg += "s";
        msg += ": " + last_err + ")";
    }
    state_->status_text = std::move(msg);
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
            if (ImGui::MenuItem("Open Comparison Config...")) {
                open_comparison_config_dialog();
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

            if (ImGui::BeginMenu("Image Loader")) {
                // Let the user compare decoding output between backends at
                // runtime.  Switching reloads all currently-open images so
                // the viewport immediately reflects the new backend.  A
                // backend entry is disabled (and annotated) when it was
                // not compiled into this build.
                auto loader_item = [&](LoaderBackend b) {
                    const bool available = ImageLoader::has_backend(b);
                    const bool selected = (state_->loader_backend == b);
                    std::string label = ImageLoader::backend_name(b);
                    if (!available) label += "  (not compiled in)";
                    if (ImGui::MenuItem(label.c_str(), nullptr, selected,
                                        available && !selected)) {
                        state_->loader_backend = b;
                        reload_all_images();
                    }
                };
                loader_item(LoaderBackend::ImageMagick);
                loader_item(LoaderBackend::OpenCV);
                ImGui::EndMenu();
            }

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

    // Group selector, only shown when a comparison-config is active.
    // Switching the combo triggers an on-demand download + load of the
    // selected group; only that one group's pixels are kept resident.
    if (state_->current_group_idx >= 0 &&
        !state_->comparison_config.groups.empty()) {
        const auto& groups = state_->comparison_config.groups;
        int idx = state_->current_group_idx;
        std::string preview = (idx >= 0 &&
                               idx < static_cast<int>(groups.size()))
            ? groups[idx].name : std::string("(none)");
        preview += " (" +
            std::to_string((idx >= 0 && idx < static_cast<int>(groups.size()))
                           ? groups[idx].items.size() : 0) + ")";
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##group", preview.c_str())) {
            for (int i = 0; i < static_cast<int>(groups.size()); ++i) {
                std::string label = groups[i].name + "  (" +
                    std::to_string(groups[i].items.size()) + ")";
                bool selected = (i == idx);
                if (ImGui::Selectable(label.c_str(), selected)) {
                    switch_to_comparison_group(i);
                }
                if (!groups[i].description.empty() &&
                    ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", groups[i].description.c_str());
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::TextDisabled("Group %d / %d",
                            idx + 1, static_cast<int>(groups.size()));
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
                // Only YUV streams can have their decoder parameters
                // re-edited after load; other sources (still images)
                // don't have user-visible parameters.
                if (dynamic_cast<YuvRawSource*>(entry.source.get())) {
                    if (ImGui::MenuItem("Edit YUV parameters...")) {
                        begin_edit_yuv_entry(i);
                    }
                    ImGui::Separator();
                }
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
            char buf[1024];
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
