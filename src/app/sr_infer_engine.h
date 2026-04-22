#ifndef IDIFF_SR_INFER_ENGINE_H
#define IDIFF_SR_INFER_ENGINE_H

#include <filesystem>
#include <memory>
#include <string>

namespace idiff {

// Status of a super-resolution inference engine.
enum class SREngineStatus {
    Idle,       // Engine is ready but not currently processing.
    Running,    // Engine is actively performing SR inference.
    Completed,  // Last inference task finished successfully.
    Failed      // Last inference task failed (check last_error()).
};

// Error information returned when a SR inference operation fails.
struct SRError {
    std::string type;        // Category, e.g. "subprocess", "io", "model"
    std::string description; // Human-readable error message.
};

// Abstract base class defining the unified interface for all
// super-resolution inference engines.  Upper-layer code (UI, image
// list, etc.) only ever touches SRInferEngine pointers — concrete
// engines are created through the factory and never referenced
// directly outside of their own implementation files.
class SRInferEngine {
public:
    virtual ~SRInferEngine() = default;

    // Launch SR inference on the given input image, writing results to
    // output_path.  Returns true if the inference was started
    // successfully; false on immediate failure (e.g. invalid parameters).
    // When the method returns true the engine status transitions to
    // Running and the caller should poll get_status() until it reaches
    // Completed or Failed.
    //
    // input_path:  source image file path (must exist).
    // output_path: destination path for the upscaled image.
    // scale:       upscale factor (2 or 4).
    // tile_size:   spatial tile size in pixels (0 = no tiling).
    // tile_overlap:overlap between adjacent tiles in pixels.
    // model:       model identifier or filename.
    // color_correction: color-correction method name.
    virtual bool start_inference(
        const std::filesystem::path& input_path,
        const std::filesystem::path& output_path,
        int scale = 2,
        int tile_size = 256,
        int tile_overlap = 64,
        const std::string& model = "seedvr2_ema_3b-Q4_K_M.gguf",
        const std::string& color_correction = "lab") = 0;

    // Query the current engine status.
    virtual SREngineStatus get_status() const = 0;

    // Retrieve the output image path (valid only after status ==
    // Completed).
    virtual std::filesystem::path get_output_path() const = 0;

    // Attempt to cancel a running inference.  Returns true if
    // cancellation was initiated successfully.
    virtual bool cancel() = 0;

    // Retrieve error information from the last failed inference.
    // Valid only when get_status() == Failed.
    virtual SRError last_error() const = 0;

    // Poll progress as a value in [0, 1].  Returns -1.0 if progress
    // reporting is not supported by the engine or the engine is idle.
    virtual float get_progress() const = 0;
};

} // namespace idiff

#endif // IDIFF_SR_INFER_ENGINE_H