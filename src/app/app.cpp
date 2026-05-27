#include "app/app.h"
#include "app/controller.h"
#include "app/status_reporter.h"
#include "app/ui/dialogs.h"
#include "app/ui/image_list.h"
#include "app/ui/inspector.h"
#include "app/ui/status_bar.h"
#include "app/ui/toolbar.h"
#include "app/ui/viewport_panel.h"
#include "app/ui/yuv_dialog.h"
#include "app/viewport.h"
#include "app/metrics_panel.h"
#include "app/pixel_inspector_panel.h"
#include "app/pixel_sampler.h"
#include "app/properties_panel.h"
#include "app/settings.h"
#include "app/sr_infer_engine.h"
#include "app/sr_infer_engine_factory.h"
#include "app/seedvr2_engine.h"
#include "app/platform/platform.h"
#include "app/sr_dialog.h"
#include "app/io/texture_uploader.h"
#include "app/io/file_dialog.h"
#include "core/file_watcher.h"
#include "domain/image_library.h"
#include "domain/selection_model.h"
#include "domain/timeline_model.h"
#include "domain/diff_service.h"
#include "domain/sr_task_service.h"
#include "domain/comparison_config_service.h"
#include "util/logger.h"

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
#include <string_view>
#include <unordered_map>
#include <utility>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "core/channel_view.h"
#include "core/depth_utils.h"
#include "core/image_loader.h"
#include "core/image_processor.h"
#include "core/image_comparator.h"
#include "core/media_source.h"
#include "core/detail/platform_utf8.h"

namespace idiff {

namespace {

// IStatusReporter implementation that writes directly into App::State.
// Constructed with raw pointers to the State fields so the reporter
// has no notion of App at all and is trivially substitutable in
// tests.  Lifetime: the reporter is owned by State; the fields it
// borrows live in the same struct so the pointers stay valid for as
// long as the reporter does.
class StateStatusReporter : public IStatusReporter {
public:
    StateStatusReporter(std::string* status_text,
                        std::string* status_msg,
                        ErrorDialogState* error_dialog) noexcept
        : status_text_(status_text),
          status_msg_(status_msg),
          error_dialog_(error_dialog) {}

    void set_status(const std::string& text) override {
        *status_text_ = text;
    }

    void append_status(const std::string& text) override {
        if (text.empty()) return;
        if (!status_text_->empty()) *status_text_ += " | ";
        *status_text_ += text;
    }

    void set_sr_status(const std::string& text) override {
        *status_msg_ = text;
    }

    void show_error(const std::string& title,
                    const std::string& message) override {
        error_dialog_->title = title;
        error_dialog_->message = message;
        error_dialog_->visible = true;
        error_dialog_->needs_open = true;
    }

private:
    std::string* status_text_;
    std::string* status_msg_;
    ErrorDialogState* error_dialog_;
};

} // namespace

struct App::State {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    std::unique_ptr<Viewport> viewport;
    std::unique_ptr<MetricsPanel> metrics_panel;
    std::unique_ptr<PropertiesPanel> properties_panel;
    std::unique_ptr<PixelInspectorPanel> pixel_panel;

    UpscaleMethod upscale_method = UpscaleMethod::Lanczos;

    std::string status_text;
    std::string status_msg;   // Last SR/notification message

    // Error notification popup state.  Visible flag, transient open
    // request, and the title / message strings rendered by the modal.
    // Mutated by IStatusReporter::show_error and by the dialog's "OK"
    // button.
    ErrorDialogState error_dialog;

    // Quit-while-SR-running confirmation dialog state.  When confirmed
    // flips to true the next frame() begins the shutdown sequence.
    QuitConfirmDialogState quit_confirm_dialog;
    bool show_inspector = true;
    bool show_image_list = true;
    int sidebar_tab = 0;

    // Difference-mode options (heatmap color scheme and amplification).
    HeatmapColor heatmap_color = HeatmapColor::Inferno;
    double diff_amplification = 5.0;

    // Persistent cross-session settings (currently just last-used YUV
    // parameters).  Loaded in App::init(), saved whenever a YUV file is
    // successfully added.
    AppSettings settings;

    // YUV-parameters dialog state.  When yuv_dialog.pending_paths is
    // non-empty, frame() opens a modal for the front element; the user
    // either confirms (turning it into a YuvRawSource entry) or skips.
    // When yuv_dialog.editing_entry_idx >= 0 the dialog is in "edit"
    // mode targeting that entry instead of loading a new file.
    YuvDialogState yuv_dialog;

    // File-watcher state.  The watcher runs a background thread that
    // monitors all loaded file paths; poll_file_watcher() drains events
    // each frame and arms the reload dialog when changes are detected.
    std::unique_ptr<FileWatcher> file_watcher;
    ReloadDialogState reload_dialog;

    // Injectable IO collaborators.  Tests substitute fakes; the
    // production wiring in App::init() installs the SDL/NFD-backed
    // implementations.  Stored here (rather than as App members) so
    // App's public header does not depend on app/io headers.
    std::unique_ptr<ITextureUploader> texture_uploader;
    std::unique_ptr<IFileDialog> file_dialog;

    // IStatusReporter implementation that forwards into the status /
    // error fields above.  Lazily constructed in App::init() so its
    // borrowed pointers always reach a stable State; cleared in
    // App::shutdown() before State is destroyed.
    std::unique_ptr<IStatusReporter> status_reporter;
};

App::App()
    : state_(std::make_unique<State>()) {}

App::~App() = default;

const std::vector<ImageEntry>& App::entries() const noexcept {
    return library_->all();
}

const std::set<int>& App::selected() const noexcept {
    return selection_->indices();
}

std::vector<ImageEntry>& App::entries_view() noexcept {
    return library_->all();
}

const std::vector<ImageEntry>& App::entries_view() const noexcept {
    return library_->all();
}

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

    // Figure out the framebuffer-to-window ratio *before* initialising
    // the renderer backend so we can rasterise the UI font at the
    // physical pixel density.  Without this step the font atlas is
    // built at logical-pixel size (e.g. 13 px) and SDL upscales the
    // glyphs to physical pixels at draw time, which produces the
    // blurry text seen on Retina displays.
    int fb_w = 0, fb_h = 0;
    SDL_GetRendererOutputSize(renderer, &fb_w, &fb_h);
    int win_w = 0, win_h = 0;
    SDL_GetWindowSize(window, &win_w, &win_h);
    float dpi_scale = (win_w > 0) ? static_cast<float>(fb_w) / win_w : 1.0f;
    if (dpi_scale < 1.0f) dpi_scale = 1.0f;

    // Pick the first readable system UI font that contains CJK glyphs,
    // so Chinese titles/labels in comparison-config JSON render as
    // real characters instead of '?' placeholders.  If none of the
    // candidates exist we silently fall back to ImGui's built-in
    // ProggyClean bitmap font (ASCII only, but never fails to load).
    const float base_font_size = 15.0f;
    ImFontConfig font_cfg;
    font_cfg.OversampleH = 2;
    font_cfg.OversampleV = 2;
    font_cfg.PixelSnapH = false;
    // Rasterise at physical-pixel size; FontGlobalScale compensates
    // below so ImGui layout keeps using logical-pixel metrics.
    const float pixel_font_size = base_font_size * dpi_scale;
    static const char* kFontCandidates[] = {
#if defined(__APPLE__)
        // Modern macOS (Big Sur+) ships PingFang here; older releases
        // fall back to the secondary Chinese-capable system fonts.
        "/System/Library/Fonts/PingFang.ttc",
        "/System/Library/Fonts/Hiragino Sans GB.ttc",
        "/System/Library/Fonts/STHeiti Medium.ttc",
        "/System/Library/Fonts/STHeiti Light.ttc",
        "/Library/Fonts/Arial Unicode.ttf",
#elif defined(_WIN32)
        "C:\\Windows\\Fonts\\msyh.ttc",
        "C:\\Windows\\Fonts\\msyh.ttf",
        "C:\\Windows\\Fonts\\simhei.ttf",
#else
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
#endif
    };
    ImFont* ui_font = nullptr;
    for (const char* path : kFontCandidates) {
        if (std::filesystem::exists(path)) {
            ui_font = io.Fonts->AddFontFromFileTTF(
                path, pixel_font_size, &font_cfg,
                io.Fonts->GetGlyphRangesChineseFull());
            if (ui_font) break;
        }
    }
    if (!ui_font) {
        // Fallback: default font, still rasterised at DPI-aware size.
        ImFontConfig default_cfg;
        default_cfg.SizePixels = pixel_font_size;
        default_cfg.OversampleH = 2;
        default_cfg.OversampleV = 2;
        io.Fonts->AddFontDefault(&default_cfg);
    }
    // Keep layout in logical pixels; the atlas is already in physical
    // pixels thanks to pixel_font_size above.
    io.FontGlobalScale = 1.0f / dpi_scale;

    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    // SDL2 keeps text input enabled by default after window creation,
    // which makes the OS IME (e.g. Pinyin on macOS / Windows) eat
    // every keystroke -- pressing M or P over the viewport pops up a
    // Chinese candidate window instead of triggering our hotkey.
    // Recent ImGui SDL2 backends no longer toggle text input
    // themselves (see imgui_impl_sdl2.cpp 2023-04-06 changelog), so
    // we manage it ourselves: start with it OFF, and re-enable it on
    // demand from frame() based on io.WantTextInput.
    SDL_StopTextInput();

    SDL_RenderSetScale(renderer, dpi_scale, dpi_scale);

    state_->viewport = std::make_unique<Viewport>();
    state_->metrics_panel = std::make_unique<MetricsPanel>();
    state_->properties_panel = std::make_unique<PropertiesPanel>();
    state_->pixel_panel = std::make_unique<PixelInspectorPanel>();

    // Load persistent settings (last-used YUV params, etc.).  A missing
    // file is fine -- AppSettings::load falls back to defaults.
    state_->settings = AppSettings::load();
    // Seed the dialog with whatever the user last confirmed so they do
    // not have to retype resolution / pixel-format for each file.
    state_->yuv_dialog.params = state_->settings.last_yuv_params;
    // Restore viewport overlay toggles from the last session.
    state_->viewport->set_show_ruler(state_->settings.show_ruler);
    state_->viewport->set_show_grid(state_->settings.show_grid);
    state_->viewport->set_grid_layout(static_cast<GridLayout>(state_->settings.grid_layout));
    state_->viewport->set_grid_cols(state_->settings.grid_cols);
    // Restore difference-mode options from the last session.
    state_->heatmap_color = static_cast<HeatmapColor>(state_->settings.heatmap_color);
    state_->diff_amplification = state_->settings.diff_amplification;

    NFD_Init();

    // Install IO interfaces backed by the real SDL renderer and the
    // initialised NFD library.  Replacing them with fakes from the
    // outside is the seam tests use to drive the App without touching
    // the platform.
    state_->texture_uploader = std::make_unique<SdlTextureUploader>(renderer);
    state_->file_dialog = std::make_unique<NfdFileDialog>();

    // Wire the status reporter to the State fields the UI already
    // reads when drawing the status bar / error dialog.  Constructed
    // here (rather than inline at AppController) so the borrowed
    // pointers stay stable for the controller's full lifetime.
    state_->status_reporter = std::make_unique<StateStatusReporter>(
        &state_->status_text,
        &state_->status_msg,
        &state_->error_dialog);

    // Bring up every domain service in one place.  The controller
    // borrows the texture uploader and status reporter by reference,
    // so both must outlive it (handled by destruction order in
    // App::shutdown / ~App).
    controller_ = std::make_unique<AppController>(
        *state_->texture_uploader, *state_->status_reporter);
    library_ = &controller_->library();
    selection_ = &controller_->selection();
    timeline_ = &controller_->timeline();
    diff_service_ = &controller_->diff();
    sr_service_ = &controller_->sr_tasks();
    comparison_config_ = &controller_->comparison_config();

    state_->file_watcher = std::make_unique<FileWatcher>();

    // Detect whether a super-resolution upscaler is available next to
    // the executable (or via SEEDVR2_UPSCALER_PATH).  Register the
    // SeedVR2 engine only when detected so the UI can hide SR controls
    // gracefully on systems without the upscaler installed.
    auto upscaler_path = platform::seedvr2_detect_upscaler();
    if (!upscaler_path.empty()) {
        sr_enabled_ = true;
        SRInferEngineFactory::instance().register_engine(
            "seedvr2",
            [upscaler_path]() -> std::unique_ptr<SRInferEngine> {
                return std::make_unique<SeedVR2Engine>(upscaler_path);
            });
    }

    return true;
}

void App::shutdown() {
    NFD_Quit();

    if (library_) {
        library_->clear();
    }

    if (diff_service_) {
        diff_service_->clear();
    }

    state_->viewport.reset();
    state_->metrics_panel.reset();
    state_->properties_panel.reset();

    // Tear down every domain service (and the SDL textures they own)
    // before the texture uploader they borrow.  Clearing the raw
    // observer pointers here keeps any post-shutdown access caught by
    // null deref instead of dangling.
    controller_.reset();
    library_ = nullptr;
    selection_ = nullptr;
    timeline_ = nullptr;
    diff_service_ = nullptr;
    sr_service_ = nullptr;
    comparison_config_ = nullptr;

    // Status reporter is borrowed by the controller, so it must be
    // released after the controller is gone.
    state_->status_reporter.reset();

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
            if (!entries_view().empty() &&
                ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) {
                save_viewport_dialog();
            }
            if (!entries_view().empty() &&
                ImGui::IsKeyPressed(ImGuiKey_F5)) {
                reload_all_images();
            }

            // Channel view shortcuts: 1-9 cycle through modes.
            if (!selection_->empty()) {
                static constexpr ChannelViewMode kModeMap[9] = {
                    ChannelViewMode::None,
                    ChannelViewMode::RGB,
                    ChannelViewMode::R,
                    ChannelViewMode::G,
                    ChannelViewMode::B,
                    ChannelViewMode::AlphaGray,
                    ChannelViewMode::AlphaContour,
                    ChannelViewMode::Y,
                    ChannelViewMode::U,
                };
                for (int i = 0; i < 9; ++i) {
                    if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_1 + i))) {
                        state_->viewport->set_channel_view_mode(kModeMap[i]);
                        for (int s : selection_->indices()) {
                            if (s >= 0 && s < static_cast<int>(entries_view().size())) {
                                entries_view()[s].texture_dirty = true;
                            }
                        }
                        break;
                    }
                }
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
        for (const auto& e : entries_view()) {
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

    if (state_->show_image_list) render_image_list();
    render_viewport();
    update_pixel_inspector_hover();
    if (state_->show_inspector) render_right_sidebar();
    render_timeline_bar();
    render_status_bar();
    render_yuv_params_dialog();
    render_error_dialog();
    render_quit_confirm_dialog();
    render_reload_dialog();
    render_sr_dialog();
    poll_sr_tasks();
    poll_file_watcher();

    ImGui::Render();

    // Sync SDL's IME / text-input state to whatever ImGui actually
    // wants this frame.  When no InputText widget is focused
    // (the common case while the user is just looking at the
    // viewport), text input stays off and the OS IME does not
    // intercept M, P, or any other plain-letter hotkey -- no
    // Chinese / Japanese candidate window pops up, no character is
    // composed.  We re-enable on demand only while a text widget is
    // focused, which is exactly when the user wants typing.
    {
        ImGuiIO& io = ImGui::GetIO();
        SDL_bool active = SDL_IsTextInputActive();
        if (io.WantTextInput && !active) {
            SDL_StartTextInput();
        } else if (!io.WantTextInput && active) {
            SDL_StopTextInput();
        }
    }

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
    // Raw YUV files carry no decoding metadata, so we cannot load
    // them synchronously through the controller.  Peel them off into
    // the YUV-parameter dialog queue first; the dialog converts each
    // path into a YuvRawSource entry on confirmation.  Everything
    // else is forwarded to the controller in one batch so it can do
    // the first-load auto-select correctly.
    std::vector<std::string> still_paths;
    still_paths.reserve(paths.size());
    for (const auto& path : paths) {
        auto dot = path.find_last_of('.');
        std::string ext_lower = (dot != std::string::npos) ? path.substr(dot)
                                                           : std::string();
        for (auto& c : ext_lower) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (ext_lower == ".yuv") {
            state_->yuv_dialog.pending_paths.push_back(path);
            state_->yuv_dialog.needs_open = true;
        } else {
            still_paths.push_back(path);
        }
    }

    if (still_paths.empty()) return;

    auto result = controller_->load_images(still_paths);

    // Register successfully-loaded paths with the file watcher so we
    // detect external modifications (e.g. mv new.png old.png).
    for (const auto& entry : entries_view()) {
        state_->file_watcher->add_path(entry.path);
    }

    // The viewport's comparison mode is owned by the UI layer; the
    // controller just tells us when its first-load auto-select fired
    // so we can put the new selection on screen immediately.
    if (result.did_first_load_select && state_->viewport) {
        state_->viewport->set_mode(ComparisonMode::Overlay);
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
    controller_->reload_all_images();
}

void App::get_ab_indices(int& a_idx, int& b_idx) const {
    controller_->get_ab_indices(a_idx, b_idx);
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

    const bool was_empty = entries_view().empty();

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

    library_->add(std::move(entry));
    state_->file_watcher->add_path(path);

    sort_entries_by_name();
    compute_display_labels();
diff_service_->mark_dirty();

    // Persist parameters so the next .yuv file starts with the same
    // defaults in the dialog.
    state_->settings.last_yuv_params = params;
    state_->settings.save();

    // Mirror the "first load" convenience from load_images(): if the
    // entry list was empty before this add, auto-select up to the first
    // two items and switch to Overlay.
    if (was_empty && !entries_view().empty()) {
        selection_->clear();
        selection_->set_swap_ab(false);
        int pick = std::min<int>(2, static_cast<int>(entries_view().size()));
        for (int i = 0; i < pick; i++) selection_->insert(i);
        for (int s : selection_->indices()) {
            if (s >= 0 && s < static_cast<int>(entries_view().size())) {
                entries_view()[s].texture_dirty = true;
            }
        }
diff_service_->mark_dirty();
        if (state_->viewport) {
            state_->viewport->set_mode(ComparisonMode::Overlay);
        }
    }

    state_->status_text = "Loaded YUV: " + path;
    return true;
}

void App::begin_edit_yuv_entry(int index) {
    if (index < 0 || index >= static_cast<int>(entries_view().size())) return;
    auto* yuv = dynamic_cast<YuvRawSource*>(entries_view()[index].source.get());
    if (!yuv) return;  // not a YUV stream; silently ignore

    // Seed the dialog with this entry's actual current parameters so the
    // user tweaks from the existing configuration rather than from
    // settings defaults or last_yuv_params.
    state_->yuv_dialog.params = yuv->params();
    state_->yuv_dialog.editing_entry_idx = index;
    state_->yuv_dialog.needs_open = true;
}

bool App::update_yuv_entry_params(int index, const YuvStreamParams& params) {
    if (index < 0 || index >= static_cast<int>(entries_view().size())) return false;
    auto& entry = entries_view()[index];

    // Build the new source first; only swap on success so a bad edit
    // leaves the existing (working) stream untouched.
    auto source = std::make_unique<YuvRawSource>(entry.path, params);
    if (source->frame_count() <= 0) {
        state_->status_text = "YUV: invalid parameters for " + entry.path;
        return false;
    }
    // Attempt to keep the current timeline position when possible so the
    // user sees what the fix did on the frame they were inspecting.
    int target_frame = timeline_->current_frame() + entry.frame_offset;
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

diff_service_->mark_dirty();

    // Remember the successful parameters as the new load-dialog default.
    state_->settings.last_yuv_params = params;
    state_->settings.save();

    state_->status_text = "Updated YUV parameters: " + entry.filename;
    return true;
}



void App::render_yuv_params_dialog() {
    YuvDialogCallbacks cb;
    cb.resolve_entry_path = [this](int idx) -> std::string {
        const auto& entries = entries_view();
        if (idx < 0 || idx >= static_cast<int>(entries.size())) return {};
        return entries[idx].path;
    };
    cb.default_load_params = [this]() {
        return state_->settings.last_yuv_params;
    };
    cb.on_load_confirm = [this](const std::string& path,
                                const YuvStreamParams& params) {
        add_yuv_entry(path, params);
    };
    cb.on_edit_apply = [this](int idx, const YuvStreamParams& params) {
        update_yuv_entry_params(idx, params);
    };
    idiff::render_yuv_params_dialog(state_->yuv_dialog, cb);
}

int App::timeline_length() const {
    return controller_->timeline_length();
}

void App::sync_entries_to_timeline() {
    controller_->sync_entries_to_timeline();
}

void App::preview_entries_to_timeline() {
    controller_->preview_entries_to_timeline();
}

float App::render_timeline_bar() {
    TimelineBarInputs in;
    in.entries = &entries_view();
    in.timeline = timeline_;
    in.on_frame_changed = [this]() { sync_entries_to_timeline(); };
    in.on_frame_preview = [this]() { preview_entries_to_timeline(); };
    return idiff::render_timeline_bar(in);
}

void App::sort_entries_by_name() {
    controller_->sort_entries_by_name();
}

void App::move_entry(int from, int to) {
    controller_->move_entry(from, to);
}

void App::open_file_dialog() {
    // A single "All supported" filter is friendlier than a
    // multi-filter drop-down: the user rarely cares whether something
    // is an image or a config, they just want to point at a file and
    // move on.  load_paths() takes care of routing after the fact.
    std::vector<FileDialogFilter> filters = {
        { "Images, videos, YUV streams, and comparison configs",
          "png,jpg,jpeg,bmp,tiff,tif,webp,dng,cr2,nef,arw,yuv,json,"
          "mp4,mkv,mov,avi,webm,flv,ts,m4v,wmv,mpg,mpeg,3gp" },
    };
    auto result = state_->file_dialog->open_multiple(filters);
    if (!result.error.empty()) {
        state_->status_text = "File dialog error: " + result.error;
        return;
    }
    if (!result.paths.empty()) {
        load_paths(result.paths);
    }
}

void App::open_comparison_config_dialog() {
    std::vector<FileDialogFilter> filters = {
        { "Comparison config (JSON)", "json" },
    };
    auto result = state_->file_dialog->open_single(filters);
    if (!result.error.empty()) {
        state_->status_text = "Config dialog error: " + result.error;
        return;
    }
    if (result.paths.empty()) return;
    load_comparison_config_from_path(result.paths.front());
}

void App::load_comparison_config_from_path(const std::string& path) {
    state_->file_watcher->clear();
    auto result = controller_->load_comparison_config(path);
    // Register the newly loaded paths with the file watcher.
    for (const auto& entry : entries_view()) {
        state_->file_watcher->add_path(entry.path);
    }
    if (result.did_first_load_select && state_->viewport) {
        state_->viewport->set_mode(ComparisonMode::Overlay);
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
    state_->file_watcher->clear();
    auto result = controller_->switch_to_comparison_group(group_idx);
    for (const auto& entry : entries_view()) {
        state_->file_watcher->add_path(entry.path);
    }
    if (result.did_first_load_select && state_->viewport) {
        state_->viewport->set_mode(ComparisonMode::Overlay);
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
        if (idx < 0 || idx >= static_cast<int>(entries_view().size())) return {};
        const auto& e = entries_view()[idx];
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
            tag ? (std::string("[") + tag + "] " + entries_view()[idx].display_label)
                : entries_view()[idx].display_label);
    };
    if (ab_idx[0] >= 0) push_slot(ab_idx[0], "A");
    if (ab_idx[1] >= 0) push_slot(ab_idx[1], "B");
    for (int s : selection_->indices()) {
        if (s == ab_idx[0] || s == ab_idx[1]) continue;
        push_slot(s, nullptr);
    }

    if (slot_mats.empty() &&
        !(mode == ComparisonMode::Difference && !diff_service_->empty())) {
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
        // Compose every diff heatmap (A vs partner_i) onto one canvas
        // using the same grid layout Viewport::render_difference draws
        // on screen, so the saved PNG matches what the user sees.
        if (diff_service_->empty()) {
            state_->status_text = "Save: no diff map available "
                                  "(select at least 2 images first)";
            return;
        }

        int n = static_cast<int>(diff_service_->size());
        int cols, rows;
        Viewport::compute_grid(n, vport.grid_layout(), vport.grid_cols(),
                               cols, rows);

        int cell_w = 0, cell_h = 0;
        for (const auto& slot : diff_service_->slots()) {
            if (!slot.image) continue;
            cell_w = std::max(cell_w, slot.image->mat().cols);
            cell_h = std::max(cell_h, slot.image->mat().rows);
        }
        if (cell_w <= 0 || cell_h <= 0) {
            state_->status_text = "Save: diff image has zero dimensions";
            return;
        }

        int out_w = cell_w * cols;
        int out_h = cell_h * rows;
        cv::Mat canvas = cv::Mat::zeros(out_h, out_w, CV_8UC4);

        for (int i = 0; i < n; i++) {
            if (!diff_service_->slots()[i].image) continue;
            cv::Mat m = to_bgra8(diff_service_->slots()[i].image->mat());
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
        Viewport::compute_grid(n, vport.grid_layout(), vport.grid_cols(),
                               cols, rows);

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
    std::vector<FileDialogFilter> filters = {
        { "PNG image", "png" },
        { "JPEG image", "jpg,jpeg" },
    };
    auto dlg = state_->file_dialog->save(filters, "viewport.png");
    if (!dlg.error.empty()) {
        state_->status_text = "Save dialog error: " + dlg.error;
        return;
    }
    if (dlg.paths.empty()) return;
    std::string path = dlg.paths.front();

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
        bool ok = false;
        {
            // cv::imwrite() uses fopen() internally which cannot handle
            // non-ASCII paths on Windows.  Encode to memory first, then
            // write via platform::write_file_binary which does the right
            // thing on every OS.
            auto dot = path.rfind('.');
            std::string ext = (dot != std::string::npos) ? path.substr(dot) : ".png";
            if (ext[0] != '.') ext = "." + ext;

            std::vector<uint8_t> buf;
            if (!cv::imencode(ext, composed, buf)) {
                state_->status_text = "Save failed (encode): " + path;
                return;
            }
            ok = platform::write_file_binary(path, buf.data(), buf.size());
        }
        state_->status_text = ok ? ("Saved viewport to: " + path)
                                 : ("Save failed: " + path);
    } catch (const cv::Exception& ex) {
        state_->status_text = std::string("Save failed: ") + ex.what();
    }
}

void App::remove_entry(int index) {
    if (index >= 0 && index < static_cast<int>(entries_view().size())) {
        const auto& path = entries_view()[index].path;
        state_->file_watcher->remove_path(path);

        // Purge from the reload dialog so a stale notification does not
        // show a file that is no longer loaded.
        auto& rd = state_->reload_dialog;
        auto& cp = rd.changed_paths;
        cp.erase(std::remove(cp.begin(), cp.end(), path), cp.end());
        if (cp.empty() && rd.visible) {
            rd.visible = false;
        }
    }
    controller_->remove_entry(index);
}

void App::compute_display_labels() {
    controller_->compute_display_labels();
}

void App::update_display_image(int index) {
    if (index < 0 || index >= static_cast<int>(entries_view().size())) return;

    auto& entry = entries_view()[index];
    if (!entry.image) return;

    int target_w = entry.image->info().width;
    int target_h = entry.image->info().height;

    for (int s : selection_->indices()) {
        if (s == index) continue;
        if (s < 0 || s >= static_cast<int>(entries_view().size())) continue;
        const auto& other = entries_view()[s];
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
diff_service_->mark_dirty();
}

void App::upload_texture(ImageEntry& entry) {
    const Image* img = entry.display_image ? entry.display_image.get() : entry.image.get();
    if (!img) return;

    const auto& mat = img->mat();
    if (mat.empty()) return;

    // Apply channel view extraction if active.
    cv::Mat channel_mat;
    const cv::Mat* source_mat = &mat;
    ChannelViewMode mode = state_->viewport->channel_view_mode();
    {
        auto extracted = extract_channel_view(mat, mode,
                            state_->viewport->view_background());
        if (extracted) {
            channel_mat = std::move(*extracted);
            source_mat = &channel_mat;
        } else if (channel_view_requires_alpha(mode)) {
            // Source has no alpha channel but mode requires one.
            // Show a placeholder so the user knows the mode is active.
            channel_mat = make_no_alpha_placeholder(mat.cols, mat.rows);
            source_mat = &channel_mat;
        }
        // Otherwise (e.g. R/G/B on grayscale) fall through to original.
    }

    int w = source_mat->cols;
    int h = source_mat->rows;

    // Convert to 8-bit RGBA for SDL texture upload. Handles any depth
    // (CV_8U, CV_16U, CV_32F) and any channel count (1, 3, 4).
    cv::Mat upload_mat = convert_to_rgba8(*source_mat);
    if (upload_mat.empty()) return;

    Uint32 sdl_format = SDL_PIXELFORMAT_RGBA32;
    (void)sdl_format;

    if (entry.texture) {
        state_->texture_uploader->destroy(entry.texture);
        entry.texture = nullptr;
    }

    if (!upload_mat.isContinuous()) {
        upload_mat = upload_mat.clone();
    }
    UploadRequest req;
    req.pixels = upload_mat.ptr<std::uint8_t>();
    req.width = w;
    req.height = h;
    req.channels = 4;
    entry.texture = state_->texture_uploader->upload(req);
    if (!entry.texture) {
        LOG_WARN("texture upload failed for entry %s",
                 entry.path.c_str());
        return;
    }

    entry.tex_w = w;
    entry.tex_h = h;
    entry.texture_dirty = false;
}

void App::render_toolbar() {
    ToolbarInputs in;
    in.viewport = state_->viewport.get();
    in.show_image_list = &state_->show_image_list;
    in.show_inspector = &state_->show_inspector;
    in.upscale_method = &state_->upscale_method;
    in.any_entries_loaded = !entries_view().empty();
    in.get_loader_backend = [this]() { return controller_->loader_backend(); };
    in.set_loader_backend = [this](LoaderBackend b) {
        controller_->set_loader_backend(b);
    };
    in.on_open_files = [this]() { open_file_dialog(); };
    in.on_open_comparison_config = [this]() { open_comparison_config_dialog(); };
    in.on_save_viewport = [this]() { save_viewport_dialog(); };
    in.on_request_quit = [this]() { request_quit(); };
    in.on_reload_all_images = [this]() { reload_all_images(); };
    in.on_view_invalidated = [this]() {
        // Sync the cached channel-view mode so render_viewport's detection
        // does not fire a redundant dirty pass on the next frame.
        last_channel_view_mode_ = state_->viewport->channel_view_mode();
        for (int s : selection_->indices()) {
            if (s >= 0 && s < static_cast<int>(entries_view().size())) {
                entries_view()[s].texture_dirty = true;
            }
        }
    };
    idiff::render_toolbar(in);
}

void App::render_image_list() {
    ImageListInputs in;
    in.entries = &entries_view();
    in.selection = selection_;
    in.diff_service = diff_service_;
    in.comparison_config = comparison_config_;
    in.sr_service = sr_service_;
    in.show_image_list = &state_->show_image_list;
    in.sr_enabled = sr_enabled_;
    in.get_ab_indices = [this](int& a, int& b) { get_ab_indices(a, b); };
    in.entry_is_yuv = [this](int idx) -> bool {
        if (idx < 0 || idx >= static_cast<int>(entries_view().size())) {
            return false;
        }
        return dynamic_cast<YuvRawSource*>(
            entries_view()[idx].source.get()) != nullptr;
    };
    in.on_open_files = [this]() { open_file_dialog(); };
    in.on_switch_comparison_group =
        [this](int g) { switch_to_comparison_group(g); };
    in.on_move_entry = [this](int from, int to) { move_entry(from, to); };
    in.on_reload_entry = [this](int idx) { controller_->reload_entry(idx); };
    in.on_reload_all = [this]() { reload_all_images(); };
    in.on_remove_entry = [this](int idx) { remove_entry(idx); };
    in.on_remove_selected = [this]() {
        auto sel = selection_->indices();
        std::vector<int> desc(sel.rbegin(), sel.rend());
        for (int idx : desc) {
            remove_entry(idx);
        }
    };
    in.on_remove_all = [this]() {
        for (int i = static_cast<int>(entries_view().size()) - 1; i >= 0; --i) {
            remove_entry(i);
        }
    };
    in.on_edit_yuv_entry = [this](int idx) { begin_edit_yuv_entry(idx); };
    in.on_open_sr_dialog = [this](int idx) {
        std::vector<std::filesystem::path> inputs;
        inputs.emplace_back(entries_view()[idx].path);
        if (!sr_dialog_) sr_dialog_ = std::make_unique<SRDialogState>();
        sr_dialog_open(*sr_dialog_, inputs, state_->settings);
    };
    in.on_select_all = [this]() {
        for (int i = 0; i < static_cast<int>(entries_view().size()); ++i) {
            selection_->insert(i);
            entries_view()[i].texture_dirty = true;
        }
        selection_->set_swap_ab(false);
        diff_service_->mark_dirty();
    };
    in.on_select_only_this = [this](int idx) {
        selection_->clear();
        selection_->insert(idx);
        for (int i = 0; i < static_cast<int>(entries_view().size()); ++i) {
            entries_view()[i].texture_dirty = true;
        }
        selection_->set_swap_ab(false);
        diff_service_->mark_dirty();
    };
    in.on_invert_selection = [this]() {
        for (int i = 0; i < static_cast<int>(entries_view().size()); ++i) {
            if (selection_->contains(i)) {
                selection_->erase(i);
            } else {
                selection_->insert(i);
            }
            entries_view()[i].texture_dirty = true;
        }
        selection_->set_swap_ab(false);
        diff_service_->mark_dirty();
    };
    in.on_unselect_all = [this]() {
        selection_->clear();
        selection_->set_swap_ab(false);
        diff_service_->mark_dirty();
    };
    idiff::render_image_list(in);
}

void App::render_viewport() {
    ViewportPanelInputs in;
    in.entries = &entries_view();
    in.selection = selection_;
    in.viewport = state_->viewport.get();
    in.diff_service = diff_service_;
    in.settings = &state_->settings;
    in.status_text = &state_->status_text;
    in.diff_amplification = &state_->diff_amplification;
    in.heatmap_color = &state_->heatmap_color;
    in.viewport_slot_to_entry = &viewport_slot_to_entry_;
    in.last_channel_view_mode = &last_channel_view_mode_;
    in.sel_drag_is_ctrl = &sel_drag_is_ctrl_;
    in.get_ab_indices = [this](int& a, int& b) { get_ab_indices(a, b); };
    in.on_update_display_image = [this](int s) { update_display_image(s); };
    in.on_upload_texture = [this](int s) { upload_texture(entries_view()[s]); };
    in.on_save_viewport = [this]() { save_viewport_dialog(); };
    in.on_shift_pin_click = [this]() {
        // The hover sample for the inspector was just refreshed by
        // render_viewport_panel via the hit-test pass; pin_current_hover
        // is a no-op when the cursor is not over an image, so clicks
        // outside any image safely fall through.  We push the latest
        // (u, v) before pinning to handle the rare case where the user
        // shift-clicked on the very first frame the viewport became
        // visible.
        update_pixel_inspector_hover();
        if (state_->pixel_panel) state_->pixel_panel->pin_current_hover();
    };
    idiff::render_viewport_panel(in);
}

void App::render_right_sidebar() {
    InspectorInputs in;
    in.entries = &entries_view();
    in.selection = selection_;
    in.viewport = state_->viewport.get();
    in.get_ab_indices = [this](int& a, int& b) { get_ab_indices(a, b); };
    in.viewport_slot_to_entry = &viewport_slot_to_entry_;
    in.properties_panel = state_->properties_panel.get();
    in.metrics_panel = state_->metrics_panel.get();
    in.pixel_panel = state_->pixel_panel.get();
    in.current_panel = &state_->settings.inspector_panel;
    in.on_panel_changed = [this]() { state_->settings.save(); };
    in.show_inspector = &state_->show_inspector;
    idiff::render_right_sidebar(in);
}

// Resolve which native image is under the cursor and forward (u, v) to
// the pixel inspector.  Mirrors the lookup in render_status_bar:
// Difference mode reads from the diff slot's image (so the heatmap's
// own resolution is used), every other mode reads from the underlying
// ImageEntry referenced by the viewport_slot_to_entry table that
// render_viewport just populated.
void App::update_pixel_inspector_hover() {
    if (!state_->pixel_panel) return;
    const Viewport* vp = state_->viewport.get();
    if (!vp || !vp->hover_valid()) {
        state_->pixel_panel->update_hover(0.0, 0.0, false);
        return;
    }

    int cell = vp->hover_cell_index();
    int px = vp->hover_pixel_x();
    int py = vp->hover_pixel_y();

    const cv::Mat* mat = nullptr;

    if (vp->mode() == ComparisonMode::Difference) {
        if (diff_service_ && cell >= 0 &&
            cell < static_cast<int>(diff_service_->size())) {
            const auto& slot = diff_service_->slots()[cell];
            if (slot.image) mat = &slot.image->mat();
        }
    } else if (cell >= 0 &&
               cell < static_cast<int>(viewport_slot_to_entry_.size())) {
        int ent = viewport_slot_to_entry_[cell];
        if (ent >= 0 && ent < static_cast<int>(entries_view().size())) {
            const auto& e = entries_view()[ent];
            if (e.image) mat = &e.image->mat();
        }
    }

    if (!mat || mat->empty() ||
        px < 0 || py < 0 ||
        px >= mat->cols || py >= mat->rows) {
        state_->pixel_panel->update_hover(0.0, 0.0, false);
        return;
    }

    double u = pixel_to_norm(px, mat->cols);
    double v = pixel_to_norm(py, mat->rows);
    state_->pixel_panel->update_hover(u, v, true);
}

void App::render_error_dialog() {
    idiff::render_error_dialog(state_->error_dialog);
}

bool App::has_running_sr_tasks() const {
    return controller_->has_running_sr_tasks();
}

void App::request_quit() {
    if (has_running_sr_tasks()) {
        // Show confirmation dialog instead of quitting immediately.
        state_->quit_confirm_dialog.visible = true;
        state_->quit_confirm_dialog.needs_open = true;
    } else {
        state_->quit_confirm_dialog.confirmed = true;
    }
}

bool App::wants_quit() const {
    return state_->quit_confirm_dialog.confirmed;
}

void App::render_quit_confirm_dialog() {
    idiff::render_quit_confirm_dialog(state_->quit_confirm_dialog,
                                      *sr_service_);
}

void App::render_reload_dialog() {
    auto& rd = state_->reload_dialog;
    idiff::render_reload_dialog(rd);

    // Act on user confirmation from the previous frame.
    if (rd.reload_requested) {
        controller_->reload_entries_by_path(rd.changed_paths);
        rd.changed_paths.clear();
        rd.reload_requested = false;
    }
}

void App::poll_file_watcher() {
    auto changed = state_->file_watcher->poll_changed();
    if (changed.empty()) return;

    // Filter to paths that are still in the library (they might
    // have been removed between the event and this poll).
    std::vector<std::string> relevant;
    for (auto& p : changed) {
        for (const auto& entry : entries_view()) {
            if (entry.path == p) {
                relevant.push_back(std::move(p));
                break;
            }
        }
    }
    if (relevant.empty()) return;

    auto& rd = state_->reload_dialog;
    // Merge into any pending notification (avoid replacing the list
    // if the dialog is already showing).
    for (auto& p : relevant) {
        bool already = false;
        for (const auto& existing : rd.changed_paths) {
            if (existing == p) { already = true; break; }
        }
        if (!already) rd.changed_paths.push_back(std::move(p));
    }
    if (!rd.visible) {
        rd.visible = true;
        rd.needs_open = true;
    }
}

void App::render_sr_dialog() {
    if (!sr_dialog_) return;
    if (sr_dialog_render(*sr_dialog_)) {
        // User confirmed the dialog — start SR tasks
        for (const auto& params : sr_dialog_->task_params) {
            start_sr_task(params);
        }
        // Persist the last-used SR settings
        state_->settings.sr_scale = sr_dialog_->scale;
        state_->settings.sr_tile_size = sr_dialog_->tile_size;
        state_->settings.sr_tile_overlap = sr_dialog_->tile_overlap;
        state_->settings.sr_model = sr_dialog_->model_buf;
        state_->settings.sr_color_correction = sr_dialog_->color_correction_buf;
        state_->settings.save();
    }
}

void App::start_sr_task(const SRTaskParams& params) {
    controller_->start_sr_task(params);
}

void App::poll_sr_tasks() {
    std::vector<SrCompletion> completions;
    std::vector<SrFailure> failures;
    sr_service_->poll(completions, failures);

    for (const auto& done : completions) {
        // Add the output image to the image list.  load_images() calls
        // sort_entries_by_name(), so indices computed before the call
        // are invalid afterwards.  We must look up entries by path.
        std::vector<std::string> paths = { done.output_path };
        load_images(paths);

        int new_idx = -1;
        for (int i = 0; i < static_cast<int>(entries_view().size()); ++i) {
            if (entries_view()[i].path == done.output_path) {
                new_idx = i;
                break;
            }
        }

        int input_idx = -1;
        for (int i = 0; i < static_cast<int>(entries_view().size()); ++i) {
            if (entries_view()[i].path == done.input_path) {
                input_idx = i;
                break;
            }
        }

        if (new_idx >= 0) {
            auto& new_entry = entries_view()[new_idx];
            const std::string input_name = (input_idx >= 0)
                ? entries_view()[input_idx].filename
                : new_entry.filename;

            // Extract scale from the output path naming convention
            // <stem>_sr_<scale>x.<ext>; default to 2x on parse failure.
            int scale = 2;
            const std::filesystem::path out_path(done.output_path);
            const auto fname = out_path.stem().string();
            const auto pos = fname.find("_sr_");
            if (pos != std::string::npos) {
                scale = std::atoi(fname.c_str() + pos + 4);
                if (scale <= 0) scale = 2;
            }
            new_entry.display_label = input_name + " (SR " +
                std::to_string(scale) + "x)";

            // Auto-select: input as A, output as B for comparison.
            selection_->clear();
            if (input_idx >= 0) {
                selection_->insert(input_idx);
            }
            selection_->insert(new_idx);
            selection_->set_swap_ab(false);
            diff_service_->mark_dirty();

            for (int s : selection_->indices()) {
                if (s >= 0 && s < static_cast<int>(entries_view().size())) {
                    entries_view()[s].texture_dirty = true;
                }
            }
        }

        state_->status_msg = done.status_msg;
    }

    for (const auto& fail : failures) {
        state_->status_msg = fail.status_msg;
        // Persistent error dialog so the user can read the message
        // before the status bar scrolls past.
        state_->status_reporter->show_error(
            "Super Resolution Failed", fail.description);
    }
}

void App::render_status_bar() {
    StatusBarInputs in;
    in.entries = &entries_view();
    in.selection = selection_;
    in.viewport = state_->viewport.get();
    in.diff_service = diff_service_;
    in.sr_service = sr_service_;
    in.viewport_slot_to_entry = &viewport_slot_to_entry_;
    in.status_text = &state_->status_text;
    in.status_msg = &state_->status_msg;
    in.get_ab_indices = [this](int& a, int& b) { get_ab_indices(a, b); };
    idiff::render_status_bar(in);
}

} // namespace idiff
