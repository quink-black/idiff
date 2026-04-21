#ifndef IDIFF_PLATFORM_H
#define IDIFF_PLATFORM_H

#include <filesystem>
#include <string>

namespace idiff {
namespace platform {

float get_dpi_scale();
std::string get_resource_path();

// Resolve the full path to the seedvr2-upscaler directory.
// Checks environment variable SEEDVR2_UPSCALER_PATH first, then
// looks for a "seedvr2-upscaler" directory next to the running
// executable.  Returns an empty path when the upscaler cannot be found.
std::filesystem::path seedvr2_detect_upscaler();

} // namespace platform
} // namespace idiff

#endif // IDIFF_PLATFORM_H
