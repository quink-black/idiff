#include "app/platform/platform.h"

#include <cstdlib>
#include <filesystem>

namespace idiff {
namespace platform {

float get_dpi_scale() {
    return 1.0f;
}

std::string get_resource_path() {
    return "";
}

std::filesystem::path get_executable_path() {
    std::error_code ec;
    auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) return p;
    return {};
}

} // namespace platform
} // namespace idiff