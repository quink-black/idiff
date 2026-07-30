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

std::filesystem::path get_executable_path() {
    uint32_t buf_size = 0;
    _NSGetExecutablePath(nullptr, &buf_size);
    if (buf_size == 0) return {};
    std::vector<char> buf(buf_size);
    if (_NSGetExecutablePath(buf.data(), &buf_size) != 0) return {};
    return std::filesystem::path(buf.data());
}

} // namespace platform
} // namespace idiff
