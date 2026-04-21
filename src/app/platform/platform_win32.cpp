#include "app/platform/platform.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <cstdlib>
#include <filesystem>

namespace idiff {
namespace platform {

float get_dpi_scale() {
#ifdef _WIN32
    return static_cast<float>(GetDpiForSystem()) / 96.0f;
#else
    return 1.0f;
#endif
}

std::string get_resource_path() {
    return "";
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
    std::filesystem::path exe_dir;
#ifdef _WIN32
    wchar_t exe_buf[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, exe_buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        exe_dir = std::filesystem::path(exe_buf).parent_path();
    }
#endif

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
