#include "app/platform/platform.h"

#ifndef _WIN32
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

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

std::filesystem::path seedvr2_detect_upscaler() {
#ifndef _WIN32
    // 1. Check environment variable first
    const char* env_path = std::getenv("SEEDVR2_UPSCALER_PATH");
    if (env_path && env_path[0]) {
        auto p = std::filesystem::path(env_path);
        if (std::filesystem::is_directory(p)) {
            return p;
        }
    }

    // 2. Check relative to executable directory
    std::filesystem::path exe_dir;
#if defined(__linux__)
    exe_dir = std::filesystem::read_symlink("/proc/self/exe").parent_path();
#elif defined(__APPLE__)
    uint32_t buf_size = 0;
    _NSGetExecutablePath(nullptr, &buf_size);
    std::vector<char> exe_buf(buf_size);
    if (_NSGetExecutablePath(exe_buf.data(), &buf_size) == 0) {
        exe_dir = std::filesystem::path(exe_buf.data()).parent_path();
    }
#endif

    if (!exe_dir.empty()) {
        auto candidate = exe_dir / "seedvr2-upscaler";
        if (std::filesystem::is_directory(candidate)) {
            return candidate;
        }
    }
#endif // !_WIN32

    return {};  // Not found
}

} // namespace platform
} // namespace idiff