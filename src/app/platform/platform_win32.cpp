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

std::filesystem::path get_executable_path() {
    wchar_t buf[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) return std::filesystem::path(buf);
    return {};
}

} // namespace platform
} // namespace idiff
