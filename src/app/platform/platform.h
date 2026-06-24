#ifndef IDIFF_PLATFORM_H
#define IDIFF_PLATFORM_H

#include <filesystem>
#include <string>

namespace idiff {
namespace platform {

float get_dpi_scale();
std::string get_resource_path();

// Full path to the running executable, or an empty path on failure.
// Used by the Restart feature to re-exec the binary; also replaces the
// ad-hoc per-platform exe-dir detection that seedvr2_detect_upscaler()
// and the window-icon loader used to inline.
std::filesystem::path get_executable_path();

// Resolve the full path to the seedvr2-upscaler directory.
// Checks environment variable SEEDVR2_UPSCALER_PATH first, then
// looks for a "seedvr2-upscaler" directory next to the running
// executable.  Returns an empty path when the upscaler cannot be found.
std::filesystem::path seedvr2_detect_upscaler();

} // namespace platform
} // namespace idiff

#endif // IDIFF_PLATFORM_H
