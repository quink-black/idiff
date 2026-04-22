#ifndef IDIFF_SR_DIALOG_H
#define IDIFF_SR_DIALOG_H

#include <filesystem>
#include <string>
#include <vector>

namespace idiff {

struct AppSettings;

// Parameters for a single SR inference task.
struct SRTaskParams {
    std::filesystem::path input_path;
    std::filesystem::path output_path;
    int scale = 2;
    int tile_size = 256;
    int tile_overlap = 64;
    std::string model = "seedvr2_ema_3b-Q4_K_M.gguf";
    std::string color_correction = "lab";
};

// State for the SR configuration dialog.
struct SRDialogState {
    bool open = false;              // Whether the dialog is visible.

    // Input files (paths + display names).
    std::vector<std::filesystem::path> input_paths;
    std::vector<std::string> input_names;

    // Editable parameters (initialised from AppSettings).
    int scale = 2;
    int tile_size = 256;
    int tile_overlap = 64;
    char model_buf[256] = {};
    char color_correction_buf[64] = {};

    // Per-image output path overrides.
    // If empty, the default naming rule is used:
    //   <input_stem>_sr_<scale>x.<ext>
    std::vector<std::string> output_path_overrides;

    // Whether the user has confirmed (clicked "Start").
    bool confirmed = false;

    // Task params for each input image (populated on confirm).
    std::vector<SRTaskParams> task_params;
};

// Generate the default output path for a given input and scale.
// Rule: same directory as input, name = <stem>_sr_<scale>x.<ext>
std::filesystem::path sr_default_output_path(
    const std::filesystem::path& input, int scale);

// Open the SR dialog pre-filled from AppSettings for the given inputs.
void sr_dialog_open(SRDialogState& state,
                    const std::vector<std::filesystem::path>& inputs,
                    const AppSettings& settings);

// Render the SR configuration dialog.  Returns true when the user
// confirms (state.confirmed is set and state.task_params is populated).
bool sr_dialog_render(SRDialogState& state);

} // namespace idiff

#endif // IDIFF_SR_DIALOG_H