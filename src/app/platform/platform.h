#ifndef IDIFF_PLATFORM_H
#define IDIFF_PLATFORM_H

#include <filesystem>
#include <string>

namespace idiff {
namespace platform {

float get_dpi_scale();
std::string get_resource_path();

// Full path to the running executable, or an empty path on failure.
// Used by the Restart feature to re-exec the binary and by the
// window-icon loader.
std::filesystem::path get_executable_path();

} // namespace platform
} // namespace idiff

#endif // IDIFF_PLATFORM_H
