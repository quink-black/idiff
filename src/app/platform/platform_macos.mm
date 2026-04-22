#include "app/platform/platform.h"

#include <AppKit/AppKit.h>
#include <mach-o/dyld.h>

#include <cstdlib>
#include <filesystem>

namespace idiff {
namespace platform {

float get_dpi_scale() {
    NSScreen* screen = [NSScreen mainScreen];
    if (screen) {
        return static_cast<float>(screen.backingScaleFactor);
    }
    return 1.0f;
}

std::string get_resource_path() {
    NSBundle* bundle = [NSBundle mainBundle];
    if (bundle) {
        return std::string([bundle resourcePath].UTF8String);
    }
    return "";
}

std::filesystem::path seedvr2_detect_upscaler() {
    const char* env_path = std::getenv("SEEDVR2_UPSCALER_PATH");
    if (env_path && env_path[0]) {
        auto p = std::filesystem::path(env_path);
        if (std::filesystem::is_directory(p)) {
            return p;
        }
    }

    std::filesystem::path exe_dir;
    uint32_t buf_size = 0;
    _NSGetExecutablePath(nullptr, &buf_size);
    std::vector<char> exe_buf(buf_size);
    if (_NSGetExecutablePath(exe_buf.data(), &buf_size) == 0) {
        exe_dir = std::filesystem::path(exe_buf.data()).parent_path();
    }

    if (!exe_dir.empty()) {
        auto candidate = exe_dir / "seedvr2-upscaler";
        if (std::filesystem::is_directory(candidate)) {
            return candidate;
        }
    }

    return {};
}

} // namespace platform
} // namespace idiff
