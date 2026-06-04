#include "app/ui/toolbar.h"

#include "app/viewport.h"
#include "core/channel_view.h"

#include <imgui.h>

#include <string>

namespace idiff {

void render_toolbar(const ToolbarInputs& in) {
    if (!ImGui::BeginMainMenuBar()) return;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open Images...", "Ctrl+O")) {
            if (in.on_open_files) in.on_open_files();
        }
        if (ImGui::MenuItem("Open Comparison Config...")) {
            if (in.on_open_comparison_config) in.on_open_comparison_config();
        }
        if (ImGui::MenuItem("Save Viewport As...", "Ctrl+S",
                             false,
                             in.any_entries_loaded)) {
            if (in.on_save_viewport) in.on_save_viewport();
        }
        if (ImGui::MenuItem("Reload All", "F5",
                             false,
                             in.any_entries_loaded)) {
            if (in.on_reload_all_images) in.on_reload_all_images();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Alt+F4")) {
            if (in.on_request_quit) in.on_request_quit();
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Image List", nullptr, in.show_image_list);
        ImGui::MenuItem("Inspector", nullptr, in.show_inspector);
        ImGui::MenuItem("Group by Name", nullptr, in.group_by_name);

        if (ImGui::BeginMenu("Image Loader")) {
            // The selectable items below set the *preferred backend
            // for general still images* (PNG / JPEG / WebP / TIFF /
            // BMP).  HEIF / AVIF is not user-selectable: those
            // formats always try FFmpeg first, then fall back to
            // ImageMagick, then OpenCV, regardless of the choice
            // here -- FFmpeg cannot decode the general formats and
            // OpenCV cannot decode HEIF/AVIF, so a single global
            // toggle would be wrong for one half of the file types.
            //
            // The FFmpeg row below is therefore informational only:
            // it shows whether the FFmpeg image backend is built in,
            // not a user choice.
            ImGui::TextDisabled("Preferred backend (PNG / JPEG / etc.)");
            ImGui::Separator();

            auto loader_item = [&](LoaderBackend b) {
                const bool available = ImageLoader::has_backend(b);
                const bool selected = in.get_loader_backend &&
                                       (in.get_loader_backend() == b);
                std::string label = ImageLoader::backend_name(b);
                if (!available) label += "  (not compiled in)";
                if (ImGui::MenuItem(label.c_str(), nullptr, selected,
                                    available && !selected)) {
                    if (in.set_loader_backend) in.set_loader_backend(b);
                    if (in.on_reload_all_images) in.on_reload_all_images();
                }
            };
            loader_item(LoaderBackend::ImageMagick);
            loader_item(LoaderBackend::OpenCV);

            ImGui::Separator();
            ImGui::TextDisabled("HEIF / AVIF (automatic)");
            {
                const bool ffmpeg_available =
                    ImageLoader::has_backend(LoaderBackend::FFmpeg);
                std::string ff_label = "FFmpeg  (preferred for HEIF / AVIF)";
                if (!ffmpeg_available) ff_label += "  (not compiled in)";
                // Disabled: this is a status row, not a chooser.
                ImGui::MenuItem(ff_label.c_str(), nullptr,
                                /*selected*/ ffmpeg_available,
                                /*enabled*/  false);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip(
                    "HEIF / AVIF files always try FFmpeg first, then\n"
                    "ImageMagick, then OpenCV. The selection above\n"
                    "only affects general formats like PNG and JPEG.");
            }
            ImGui::EndMenu();
        }

        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Upscale")) {
        UpscaleMethod& method = *in.upscale_method;
        bool is_nearest  = method == UpscaleMethod::Nearest;
        bool is_bilinear = method == UpscaleMethod::Bilinear;
        bool is_bicubic  = method == UpscaleMethod::Bicubic;
        bool is_lanczos  = method == UpscaleMethod::Lanczos;

        if (ImGui::Checkbox("Nearest", &is_nearest) && is_nearest) {
            method = UpscaleMethod::Nearest;
        }
        if (ImGui::Checkbox("Bilinear", &is_bilinear) && is_bilinear) {
            method = UpscaleMethod::Bilinear;
        }
        if (ImGui::Checkbox("Bicubic", &is_bicubic) && is_bicubic) {
            method = UpscaleMethod::Bicubic;
        }
        if (ImGui::Checkbox("Lanczos", &is_lanczos) && is_lanczos) {
            method = UpscaleMethod::Lanczos;
        }

        ImGui::EndMenu();
    }

    ImGui::Separator();
    if (ImGui::SmallButton("+ Open")) {
        if (in.on_open_files) in.on_open_files();
    }
    if (in.any_entries_loaded) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Reload")) {
            if (in.on_reload_all_images) in.on_reload_all_images();
        }
    }

    // Channel view selector.
    {
        ImGui::SameLine();
        ImGui::Spacing();
        ImGui::SameLine();
        ChannelViewMode cv_mode = in.viewport->channel_view_mode();
        const char* preview = channel_view_mode_label(cv_mode);
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::BeginCombo("##channel_view", preview)) {
            static constexpr ChannelViewMode modes[] = {
                ChannelViewMode::None,
                ChannelViewMode::RGB,
                ChannelViewMode::R,
                ChannelViewMode::G,
                ChannelViewMode::B,
                ChannelViewMode::AlphaGray,
                ChannelViewMode::AlphaContour,
                ChannelViewMode::Y,
                ChannelViewMode::U,
                ChannelViewMode::V,
            };
            for (auto m : modes) {
                bool is_selected = (cv_mode == m);
                if (ImGui::Selectable(channel_view_mode_label(m), is_selected)) {
                    in.viewport->set_channel_view_mode(m);
                    if (in.on_view_invalidated) in.on_view_invalidated();
                }
                if (is_selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Channel");
    }

    // Background selector for RGBA compositing.
    {
        ImGui::SameLine();
        ImGui::Spacing();
        ImGui::SameLine();
        ViewBackground cur_bg = in.viewport->view_background();
        const char* bg_preview = view_background_label(cur_bg);
        ImGui::SetNextItemWidth(110.0f);
        if (ImGui::BeginCombo("##view_bg", bg_preview)) {
            static constexpr ViewBackground bgs[] = {
                ViewBackground::Black,
                ViewBackground::White,
                ViewBackground::Red,
                ViewBackground::Green,
                ViewBackground::Blue,
                ViewBackground::DarkChecker,
                ViewBackground::LightChecker,
            };
            for (auto b : bgs) {
                bool is_selected = (cur_bg == b);
                if (ImGui::Selectable(view_background_label(b), is_selected)) {
                    in.viewport->set_view_background(b);
                    if (in.on_view_invalidated) in.on_view_invalidated();
                }
                if (is_selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("BG");
    }

    ImGui::EndMainMenuBar();
}

} // namespace idiff
