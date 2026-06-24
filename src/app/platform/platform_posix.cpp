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

std::filesystem::path seedvr2_detect_upscaler() {
    // 1. Check environment variable first
    const char* env_path = std::getenv("SEEDVR2_UPSCALER_PATH");
    if (env_path && env_path[0]) {
        auto p = std::filesystem::path(env_path);
        if (std::filesystem::is_directory(p)) {
            return p;
        }
    }

    // 2. Check relative to executable directory
    auto exe_dir = get_executable_path().parent_path();
    if (!exe_dir.empty()) {
        auto candidate = exe_dir / "seedvr2-upscaler";
        if (std::filesystem::is_directory(candidate)) {
            return candidate;
        }
    }

    return {};  // Not found
}

} // namespace platform
} // namespace idiff