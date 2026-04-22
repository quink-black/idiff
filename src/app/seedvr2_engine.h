#ifndef IDIFF_SEEDVR2_ENGINE_H
#define IDIFF_SEEDVR2_ENGINE_H

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include "app/sr_infer_engine.h"

namespace idiff {

// SeedVR2-specific SR inference engine.  Launches the external
// seedvr2-upscaler tool as a subprocess and monitors its execution.
//
// The seedvr2-upscaler directory is resolved via:
//   1. Environment variable SEEDVR2_UPSCALER_PATH
//   2. Relative to the executable directory (seedvr2-upscaler/ subdir)
//
// The subprocess invoked is:
//   <upscaler_path>/python/python.exe <upscaler_path>/app/upscale.py
// with appropriate CLI arguments.
class SeedVR2Engine : public SRInferEngine {
public:
    // upscaler_path: root directory of seedvr2-upscaler (contains
    // python/ and app/ subdirs).
    explicit SeedVR2Engine(const std::filesystem::path& upscaler_path);

    ~SeedVR2Engine() override;

    bool start_inference(
        const std::filesystem::path& input_path,
        const std::filesystem::path& output_path,
        int scale = 2,
        int tile_size = 256,
        int tile_overlap = 64,
        const std::string& model = "seedvr2_ema_3b-Q4_K_M.gguf",
        const std::string& color_correction = "lab") override;

    SREngineStatus get_status() const override;
    std::filesystem::path get_output_path() const override;
    bool cancel() override;
    SRError last_error() const override;
    float get_progress() const override;

    // Static helper: resolve the seedvr2-upscaler path using env var
    // or relative-to-exe detection.  Returns empty path if not found.
    static std::filesystem::path resolve_upscaler_path();

    // Parse progress information from subprocess stdout/stderr lines.
    // Returns a value in [0, 1] or -1.0 if no progress info available.
    // Recognises several formats:
    //   "Progress: 42.5%"                 — explicit percentage
    //   "progress:0.425"                  — fractional value
    //   "Processing tile 2/4 [256x256]"   — tile-based progress
    static float parse_progress(const std::string& line);

private:
    // Build the full command line to invoke the upscaler subprocess.
    std::string build_command(
        const std::filesystem::path& input_path,
        const std::filesystem::path& output_path,
        int scale, int tile_size, int tile_overlap,
        const std::string& model,
        const std::string& color_correction) const;



    std::filesystem::path upscaler_path_;
    std::atomic<SREngineStatus> status_ = SREngineStatus::Idle;
    mutable std::mutex error_mutex_;
    SRError last_error_;
    std::filesystem::path output_path_;
    std::atomic<float> progress_ = -1.0f;

    // Captured subprocess stderr output for error reporting.
    std::string stderr_output_;

    // Platform-specific subprocess handle (void* to be cast to
    // HANDLE on Windows or pid_t on POSIX).  Protected by
    // subprocess_mutex_ because cancel() on the main thread and
    // the worker thread both access it.
    std::mutex subprocess_mutex_;
    void* subprocess_handle_ = nullptr;

    // Background worker thread that runs the subprocess and reads
    // its output.  Must be joined before the object is destroyed.
    std::thread worker_thread_;

    // Set by cancel() to signal the worker thread to stop reading
    // pipes and skip post-process cleanup.
    std::atomic<bool> cancel_requested_ = false;
};

} // namespace idiff

#endif // IDIFF_SEEDVR2_ENGINE_H