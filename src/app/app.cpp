#include "app/app.h"
#include "app/controller.h"
#include "app/status_reporter.h"
#include "app/ui/dialogs.h"
#include "app/ui/image_list.h"
#include "app/ui/status_bar.h"
#include "app/ui/toolbar.h"
#include "app/ui/yuv_dialog.h"
#include "app/viewport.h"
#include "app/metrics_panel.h"
#include "app/properties_panel.h"
#include "app/settings.h"
#include "app/sr_infer_engine.h"
#include "app/sr_infer_engine_factory.h"
#include "app/seedvr2_engine.h"
#include "app/platform/platform.h"
#include "app/sr_dialog.h"
#include "app/io/texture_uploader.h"
#include "app/io/file_dialog.h"
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

    SDL_RenderSetScale(renderer, dpi_scale, dpi_scale);

    state_->viewport = std::make_unique<Viewport>();
    state_->metrics_panel = std::make_unique<MetricsPanel>();
    state_->properties_panel = std::make_unique<PropertiesPanel>();

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
    if (state_->show_inspector) render_right_sidebar();
    render_timeline_bar();
    render_status_bar();
    render_yuv_params_dialog();
    render_error_dialog();
    render_quit_confirm_dialog();
    render_sr_dialog();
    poll_sr_tasks();

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

float App::render_timeline_bar() {
    TimelineBarInputs in;
    in.entries = &entries_view();
    in.timeline = timeline_;
    in.on_frame_changed = [this]() { sync_entries_to_timeline(); };
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
        { "Images, YUV streams, and comparison configs",
          "png,jpg,jpeg,bmp,tiff,tif,webp,dng,cr2,nef,arw,yuv,json" },
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
    auto result = controller_->load_comparison_config(path);
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
    auto result = controller_->switch_to_comparison_group(group_idx);
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
    int channels = source_mat->channels();

    // Metal backend does not reliably support SDL_PIXELFORMAT_RGB24 (3-byte).
    // Always convert to RGBA32 for upload to avoid rendering artifacts.
    cv::Mat upload_mat;
    if (channels == 3) {
        cv::cvtColor(*source_mat, upload_mat, cv::COLOR_RGB2RGBA);
        channels = 4;
    } else if (channels == 4) {
        upload_mat = *source_mat;
    } else if (channels == 1) {
        cv::cvtColor(*source_mat, upload_mat, cv::COLOR_GRAY2RGBA);
        channels = 4;
    } else {
        return;
    }

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
    req.channels = channels;
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
    in.on_remove_entry = [this](int idx) { remove_entry(idx); };
    in.on_edit_yuv_entry = [this](int idx) { begin_edit_yuv_entry(idx); };
    in.on_open_sr_dialog = [this](int idx) {
        std::vector<std::filesystem::path> inputs;
        inputs.emplace_back(entries_view()[idx].path);
        if (!sr_dialog_) sr_dialog_ = std::make_unique<SRDialogState>();
        sr_dialog_open(*sr_dialog_, inputs, state_->settings);
    };
    idiff::render_image_list(in);
}

void App::render_viewport() {
    // Snapshot the diff service's dirty flag at the top of the frame:
    // diff_service_->update() below clears it, but the measurement
    // reload path further down still needs to know whether the image
    // set or selection just changed.
    bool selection_changed = diff_service_->is_dirty();

    // When the image set or selection changes, clear the viewport's
    // measurement display and reload from the new entries.  Measurements
    // are persisted per-entry and synced immediately on create/delete,
    // so no "save back" step is needed here.
    if (selection_changed && state_->viewport) {
        state_->viewport->clear_measurements();
    }

    // Upload dirty textures for selected images
    for (int s : selection_->indices()) {
        if (s >= 0 && s < static_cast<int>(entries_view().size())) {
            if (entries_view()[s].texture_dirty) {
                update_display_image(s);
                upload_texture(entries_view()[s]);
            }
        }
    }

    {
        DiffService::Options opts;
        opts.amplification = state_->diff_amplification;
        opts.heatmap_color = state_->heatmap_color;
        diff_service_->update(entries_view(), *selection_, opts, state_->status_text);
    }

    // Build texture list from selected images. Place A then B in the first
    // two slots (honoring the swap flag), followed by any additional
    // selected images in their natural order.
    std::vector<SDL_Texture*> tex_ptrs;
    std::vector<int> tex_ws, tex_hs;
    std::vector<const char*> labels;
    // Hold label storage so const char* remains valid for the frame
    std::vector<std::string> label_storage;
    label_storage.reserve(selection_->size());

    // Reset the slot->entry mapping; repopulated below in lockstep with the
    // vectors above so render_status_bar can map hovered slots back.
    viewport_slot_to_entry_.clear();
    viewport_slot_to_entry_.reserve(selection_->size());

    int ab_idx[2] = {-1, -1};
    get_ab_indices(ab_idx[0], ab_idx[1]);

    auto push_entry = [&](int s, const char* prefix) {
        if (s < 0 || s >= static_cast<int>(entries_view().size())) return;
        const auto& e = entries_view()[s];
        tex_ptrs.push_back(e.texture);
        // Report the source image's native pixel dimensions, not the SDL
        // texture size.  When two selected images differ in resolution,
        // update_display_image upscales the smaller one to max(W, H) for
        // pixel-aligned diffing, which inflates entry.tex_w/tex_h.  Using
        // those inflated values would make rulers and measurements report
        // sizes in the upscaled coordinate system (e.g. a 110 px feature
        // on a 2520-wide image would read as 220 px when the partner is
        // 5040-wide).  The source image keeps the original dimensions.
        int src_w = e.image ? e.image->info().width  : e.tex_w;
        int src_h = e.image ? e.image->info().height : e.tex_h;
        tex_ws.push_back(src_w);
        tex_hs.push_back(src_h);
        std::string lbl = prefix
            ? (std::string("[") + prefix + "] " + e.display_label)
            : e.display_label;
        label_storage.push_back(std::move(lbl));
        labels.push_back(label_storage.back().c_str());
        viewport_slot_to_entry_.push_back(s);
    };

    if (ab_idx[0] >= 0) push_entry(ab_idx[0], "A");
    if (ab_idx[1] >= 0) push_entry(ab_idx[1], "B");
    for (int s : selection_->indices()) {
        if (s == ab_idx[0] || s == ab_idx[1]) continue;
        push_entry(s, nullptr);
    }

    // Load saved measurements from the newly-mapped entries into the
    // viewport.  source_cell_index is rewritten to the current slot
    // position so the viewport can project the rectangles correctly.
    if (selection_changed && state_->viewport) {
        auto& vp = *state_->viewport;
        int max_id = 1;
        for (int slot = 0; slot < static_cast<int>(viewport_slot_to_entry_.size()); slot++) {
            int entry_idx = viewport_slot_to_entry_[slot];
            if (entry_idx >= 0 && entry_idx < static_cast<int>(entries_view().size())) {
                for (const auto& m : entries_view()[entry_idx].measurements) {
                    Measurement copy = m;
                    copy.source_cell_index = slot;
                    vp.add_measurement(copy);
                }
                max_id = std::max(max_id, entries_view()[entry_idx].next_measurement_id);
            }
        }
        vp.set_next_measurement_id(max_id);
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
    // Suppressed while a right-click selection rectangle is being drawn so
    // the two gestures never overlap.
    if (hovered && !vp.selecting() &&
        ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
        ImVec2 delta = io.MouseDelta;
        vp.set_pan(vp.pan_x() + delta.x, vp.pan_y() + delta.y);
    }

    // --- Left-mouse drag to pan ---
    // In Overlay mode, the InvisibleButton over the viewport area captures
    // left-mouse drags for the A/B slider.  Only pan when that slider is
    // NOT being dragged, no selection is in progress, and Ctrl is NOT held
    // (Ctrl+left-drag is selection-zoom), so all interactions remain
    // mutually exclusive.  Also skip when Measure mode is active: the
    // left-drag is handled by the measurement block below.
    if (hovered && !vp.selecting() && !io.KeyCtrl &&
        !vp.measure_mode() && !vp.measuring() &&
        ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        bool overlay_slider_active = vp.overlay_slider_dragging();
        if (!overlay_slider_active) {
            ImVec2 delta = io.MouseDelta;
            vp.set_pan(vp.pan_x() + delta.x, vp.pan_y() + delta.y);
        }
    }

    // --- Measurement drag (Measure mode, left button) ---
    // Mutually exclusive with pan and with selection-zoom.  The drag is
    // anchored on mouse-down at the cell under the cursor and that cell's
    // source-tex dimensions are frozen for the lifetime of the drag, so
    // dragging past the cell edge extrapolates in the source image's
    // coordinate system instead of re-resolving to another cell.
    //
    // Measure mode is a hold-to-activate toggle driven by the M key (no
    // checkbox in the toolbar).  Holding M arms the next left-drag as a
    // measurement; releasing M during a drag does NOT abort, so the user
    // can let go of the key once the drag has started.  This avoids the
    // accidental measurement rectangles that a sticky checkbox produced.
    // WantTextInput (not WantCaptureKeyboard) is the correct guard:
    // WantCaptureKeyboard is true whenever any ImGui window is focused,
    // which includes the viewport itself and would block the hotkey
    // entirely.  WantTextInput is true only while a text field is
    // actively receiving input, matching the Ctrl+O guard above.
    bool measure_armed = !io.WantTextInput &&
                         ImGui::IsKeyDown(ImGuiKey_M);
    if (measure_armed != vp.measure_mode()) {
        vp.set_measure_mode(measure_armed);
        if (!measure_armed && !vp.measuring()) {
            vp.cancel_measurement();
        }
    }
    if (vp.measure_mode() || vp.measuring()) {
        bool click_left = hovered && !io.KeyCtrl &&
                          ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        // Clicking directly on an existing measurement's x button removes
        // it; do not also start a new drag from that click position.
        if (click_left && !vp.hover_measurement_close_hot() && !vp.selecting()) {
            vp.begin_measurement(io.MousePos);
        }
        if (vp.measuring()) {
            vp.update_measurement(io.MousePos);
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                const Measurement* committed = vp.end_measurement();
                // Sync the new measurement to its source entry so it
                // persists across selection changes.
                if (committed) {
                    int slot = committed->source_cell_index;
                    if (slot >= 0 && slot < static_cast<int>(viewport_slot_to_entry_.size())) {
                        int entry_idx = viewport_slot_to_entry_[slot];
                        if (entry_idx >= 0 && entry_idx < static_cast<int>(entries_view().size())) {
                            entries_view()[entry_idx].measurements.push_back(*committed);
                            entries_view()[entry_idx].next_measurement_id =
                                std::max(entries_view()[entry_idx].next_measurement_id,
                                         committed->id + 1);
                        }
                    }
                }
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                vp.cancel_measurement();
            }
        }
    }

    // --- Selection rectangle zoom (right-drag OR Ctrl+left-drag) ---
    // Ctrl+left-drag gives trackpad users a way to zoom-to-selection on
    // macOS where right-click drag is not naturally available.
    bool sel_start_right = ImGui::IsMouseClicked(ImGuiMouseButton_Right) && hovered;
    bool sel_start_ctrl  = ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hovered && io.KeyCtrl;
    if (hovered || vp.selecting()) {
        if (!vp.selecting() && (sel_start_right || sel_start_ctrl)) {
            vp.begin_selection(io.MousePos);
            sel_drag_is_ctrl_ = sel_start_ctrl;
        }
        if (vp.selecting()) {
            vp.update_selection(io.MousePos);
            bool released = sel_drag_is_ctrl_
                ? ImGui::IsMouseReleased(ImGuiMouseButton_Left)
                : ImGui::IsMouseReleased(ImGuiMouseButton_Right);
            if (released) {
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
        bool ctrl = io.KeyCtrl;
        // '0' or Ctrl+0 : fit to content
        if (ImGui::IsKeyPressed(ImGuiKey_0) || ImGui::IsKeyPressed(ImGuiKey_Keypad0)) {
            vp.fit_to_content();
        }
        // 'F' : fit to content
        if (ImGui::IsKeyPressed(ImGuiKey_F) && !ctrl) {
            vp.fit_to_content();
        }
        // Number keys for channel view are handled in the global shortcut
        // block above so they work even when the Viewport is not focused.
    }

    // --- Double-click to fit ---
    // Disabled in Measure mode: left-clicks there belong to the
    // measurement drag interaction and a surprise fit would wipe the
    // context the user is trying to measure in.
    if (hovered && !vp.measure_mode() &&
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
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

        // Grid layout selector (only meaningful in Split/Diff modes)
        ComparisonMode current_mode = static_cast<ComparisonMode>(mode_int);
        if (current_mode == ComparisonMode::Split || current_mode == ComparisonMode::Difference) {
            ImGui::SameLine();
            ImGui::Spacing();
            ImGui::SameLine();
            ImGui::Text("Layout:");
            ImGui::SameLine();
            GridLayout gl = vp.grid_layout();
            int gl_int = static_cast<int>(gl);
            ImGui::PushItemWidth(70);
            if (ImGui::Combo("##layout", &gl_int, "Auto\0Row\0Col\0NxM\0")) {
                gl = static_cast<GridLayout>(gl_int);
                vp.set_grid_layout(gl);
                state_->settings.grid_layout = gl_int;
                state_->settings.save();
            }
            ImGui::PopItemWidth();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Grid layout: Auto (heuristic), Row (1xN), Col (Nx1), NxM (custom columns)");
            }

            if (gl == GridLayout::RowsCols) {
                ImGui::SameLine();
                int cols = vp.grid_cols();
                ImGui::PushItemWidth(40);
                if (ImGui::InputInt("C##grid_cols", &cols, 1, 1)) {
                    vp.set_grid_cols(cols);
                    state_->settings.grid_cols = vp.grid_cols();
                    state_->settings.save();
                }
                ImGui::PopItemWidth();
            }
        }

        // Difference-mode options (heatmap color + amplification)
        if (current_mode == ComparisonMode::Difference) {
            ImGui::SameLine();
            ImGui::Spacing();
            ImGui::SameLine();
            ImGui::Text("Heatmap:");
            ImGui::SameLine();
            int hc_int = static_cast<int>(state_->heatmap_color);
            ImGui::PushItemWidth(70);
            if (ImGui::Combo("##heatmap_color", &hc_int, "Gray\0Inferno\0Viridis\0Coolwarm\0")) {
                state_->heatmap_color = static_cast<HeatmapColor>(hc_int);
                diff_service_->mark_dirty();
                state_->settings.heatmap_color = hc_int;
                state_->settings.save();
            }
            ImGui::PopItemWidth();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Heatmap color scheme");
            }

            ImGui::SameLine();
            float amp = static_cast<float>(state_->diff_amplification);
            ImGui::PushItemWidth(60);
            if (ImGui::SliderFloat("Amp##diff_amp", &amp, 1.0f, 50.0f, "%.1fx")) {
                state_->diff_amplification = static_cast<double>(amp);
                diff_service_->mark_dirty();
                state_->settings.diff_amplification = state_->diff_amplification;
                state_->settings.save();
            }
            ImGui::PopItemWidth();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Difference amplification factor");
            }
        }

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
        bool can_save = !entries_view().empty() &&
                        (!selection_->empty() ||
                         (vp.mode() == ComparisonMode::Difference && !diff_service_->empty()));
        ImGui::BeginDisabled(!can_save);
        if (ImGui::SmallButton("Save...")) {
            save_viewport_dialog();
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Save the current viewport (Split / Overlay / Diff) "
                              "to a PNG or JPEG file");
        }

        int sel_count = static_cast<int>(selection_->size());
        if (sel_count >= 2) {
            ImGui::SameLine();
            ImGui::Spacing();
            ImGui::SameLine();
            if (ImGui::SmallButton("Swap A/B")) {
                selection_->toggle_swap_ab();
diff_service_->mark_dirty();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Swap which selected image acts as A and B");
            }
        }

        ImGui::SameLine();
        ImGui::Spacing();
        ImGui::SameLine();

        {
            bool ruler = vp.show_ruler();
            if (ImGui::Checkbox("Ruler", &ruler)) {
                vp.set_show_ruler(ruler);
                state_->settings.show_ruler = ruler;
                state_->settings.save();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Show coordinate rulers along image edges");
            }
        }
        ImGui::SameLine();
        {
            bool grid = vp.show_grid();
            if (ImGui::Checkbox("Grid", &grid)) {
                vp.set_show_grid(grid);
                state_->settings.show_grid = grid;
                state_->settings.save();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Show grid overlay on images");
            }
        }

        // --- Measurement mode ---
        // Measurement is a hold-to-activate gesture: hold M and left-drag
        // a region.  No persistent checkbox in the toolbar -- a sticky
        // toggle was too easy to leave on, causing stray clicks to draw
        // unwanted measurement rectangles.
        if (!vp.measurements().empty()) {
            ImGui::SameLine();
            ImGui::Spacing();
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear Measurements")) {
                vp.clear_measurements();
                // Also clear from the owning entries so they don't
                // reappear on the next selection change.
                for (int entry_idx : viewport_slot_to_entry_) {
                    if (entry_idx >= 0 && entry_idx < static_cast<int>(entries_view().size())) {
                        entries_view()[entry_idx].measurements.clear();
                    }
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Remove all saved measurements");
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

        // Hint about mouse interactions
        if (vp.measure_mode()) {
            ImGui::TextColored(ImVec4(0xFF/255.0f, 0xC8/255.0f, 0x32/255.0f, 1.00f),
                               "Measure (M held): left-drag a region | "
                               "Right-drag / Ctrl+drag: zoom to selection | "
                               "Hover a rect and click x to remove");
        } else {
            ImGui::TextColored(ImVec4(0.60f, 0.60f, 0.60f, 1.00f),
                               "Drag: pan | Right-drag / Ctrl+drag: zoom to selection | "
                               "Hold M + drag: measure region");
        }
    }

    // Build diff texture/label vectors for Difference mode.  Each slot
    // is "A vs <partner>", in the same order DiffService::update()
    // populates the slot list, so metrics rows and viewport cells align.
    std::vector<SDL_Texture*> diff_tex_ptrs;
    std::vector<int> diff_tex_ws, diff_tex_hs;
    std::vector<const char*> diff_labels;
    std::vector<std::string> diff_label_storage;
    diff_tex_ptrs.reserve(diff_service_->size());
    diff_tex_ws.reserve(diff_service_->size());
    diff_tex_hs.reserve(diff_service_->size());
    diff_labels.reserve(diff_service_->size());
    diff_label_storage.reserve(diff_service_->size());
    {
        int a_lbl_idx = -1, b_unused = -1;
        get_ab_indices(a_lbl_idx, b_unused);
        std::string a_name = (a_lbl_idx >= 0 &&
                              a_lbl_idx < static_cast<int>(entries_view().size()))
                                  ? entries_view()[a_lbl_idx].display_label
                                  : std::string("A");
        for (const auto& slot : diff_service_->slots()) {
            diff_tex_ptrs.push_back(slot.texture);
            diff_tex_ws.push_back(slot.tex_w);
            diff_tex_hs.push_back(slot.tex_h);
            std::string partner_name;
            if (slot.partner_entry_idx >= 0 &&
                slot.partner_entry_idx < static_cast<int>(entries_view().size())) {
                partner_name = entries_view()[slot.partner_entry_idx].display_label;
            } else {
                partner_name = "?";
            }
            diff_label_storage.push_back("Diff: " + a_name + " vs " + partner_name);
            diff_labels.push_back(diff_label_storage.back().c_str());
        }
    }

    // Render viewport content
    vp.render(tex_ptrs, tex_ws, tex_hs, labels,
              diff_tex_ptrs, diff_tex_ws, diff_tex_hs, diff_labels);

    // Detect channel view mode changes triggered inside the Viewport
    // combo and mark selected entries dirty so textures are re-uploaded
    // with the new channel extraction on the next frame.
    if (vp.channel_view_mode() != last_channel_view_mode_) {
        last_channel_view_mode_ = vp.channel_view_mode();
        for (int s : selection_->indices()) {
            if (s >= 0 && s < static_cast<int>(entries_view().size())) {
                entries_view()[s].texture_dirty = true;
            }
        }
    }

    ImGui::End();
    ImGui::PopStyleVar();

    // Sync measurement deletions from the viewport (x button clicks
    // processed during draw_measurements) back to the owning entries.
    int deleted_id;
    while ((deleted_id = vp.consume_pending_delete()) >= 0) {
        for (auto& entry : entries_view()) {
            auto& ms = entry.measurements;
            ms.erase(std::remove_if(ms.begin(), ms.end(),
                                    [deleted_id](const Measurement& m) {
                                        return m.id == deleted_id;
                                    }),
                     ms.end());
        }
    }
}

void App::render_right_sidebar() {
    ImGui::SetNextWindowSize(ImVec2(300, 400), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Inspector", &state_->show_inspector)) {
        ImGui::End();
        return;
    }

    // Resolve A and B via the shared helper so the inspector matches the
    // viewport / image list labeling (including the Swap A/B toggle).
    int ab_idx[2] = {-1, -1};
    get_ab_indices(ab_idx[0], ab_idx[1]);

    auto get_entry = [&](int idx) -> const ImageEntry* {
        if (idx < 0 || idx >= static_cast<int>(entries_view().size())) return nullptr;
        return &entries_view()[idx];
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
                // Multi-image metrics: compare A against every other
                // selected entry (B, C, D, ...).  Rows follow the same
                // order as the viewport cells and as diff_service_->slots(), so the
                // visual/spatial layout and the metrics table stay in
                // lockstep.
                std::vector<std::pair<std::string, const Image*>> partners;
                auto add_partner = [&](int idx) {
                    if (idx < 0 ||
                        idx >= static_cast<int>(entries_view().size())) return;
                    const auto& e = entries_view()[idx];
                    const Image* disp = e.display_image
                                            ? e.display_image.get()
                                            : e.image.get();
                    if (!disp) return;
                    partners.emplace_back(e.display_label, disp);
                };
                if (ab_idx[1] >= 0) add_partner(ab_idx[1]);
                for (int s : selection_->indices()) {
                    if (s == ab_idx[0] || s == ab_idx[1]) continue;
                    add_partner(s);
                }
                state_->metrics_panel->render_pair_metrics(disp_a, partners);
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Statistics")) {
            if (state_->metrics_panel) {
                // Gather all selected images with their names for per-image stats
                std::vector<std::pair<std::string, const Image*>> stat_images;

                // A first, then B, then remaining selected (same order as viewport)
                auto add_entry = [&](int idx, const char* prefix) {
                    if (idx < 0 || idx >= static_cast<int>(entries_view().size())) return;
                    const auto& e = entries_view()[idx];
                    const Image* disp = e.display_image ? e.display_image.get()
                                                        : e.image.get();
                    if (!disp) return;
                    std::string label = prefix
                        ? (std::string("[") + prefix + "] " + e.display_label)
                        : e.display_label;
                    stat_images.emplace_back(std::move(label), disp);
                };

                add_entry(ab_idx[0], "A");
                add_entry(ab_idx[1], "B");
                for (int s : selection_->indices()) {
                    if (s == ab_idx[0] || s == ab_idx[1]) continue;
                    add_entry(s, nullptr);
                }

                state_->metrics_panel->render_statistics(stat_images);
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Measurements")) {
            if (state_->properties_panel) {
                // Build slot labels in the same order viewport_slot_to_entry_
                // was populated by render_viewport, so Measurement.source_cell_index
                // resolves to the right image name.  Keep the strings alive
                // for the duration of the call.
                std::vector<std::string> slot_storage;
                std::vector<const char*> slot_labels;
                slot_storage.reserve(viewport_slot_to_entry_.size());
                slot_labels.reserve(viewport_slot_to_entry_.size());
                for (int entry_idx : viewport_slot_to_entry_) {
                    if (entry_idx >= 0 &&
                        entry_idx < static_cast<int>(entries_view().size())) {
                        slot_storage.push_back(entries_view()[entry_idx].display_label);
                    } else {
                        slot_storage.emplace_back("(unknown)");
                    }
                    slot_labels.push_back(slot_storage.back().c_str());
                }
                std::vector<int> deleted_ids;
                bool clear_all = false;
                state_->properties_panel->render_measurements(
                    *state_->viewport, slot_labels, deleted_ids, clear_all);
                // Sync deletions back to the owning entries.
                if (clear_all) {
                    for (int entry_idx : viewport_slot_to_entry_) {
                        if (entry_idx >= 0 && entry_idx < static_cast<int>(entries_view().size())) {
                            entries_view()[entry_idx].measurements.clear();
                        }
                    }
                }
                for (int del_id : deleted_ids) {
                    for (auto& entry : entries_view()) {
                        auto& ms = entry.measurements;
                        ms.erase(std::remove_if(ms.begin(), ms.end(),
                                                [del_id](const Measurement& m) {
                                                    return m.id == del_id;
                                                }),
                                 ms.end());
                    }
                }
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
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
