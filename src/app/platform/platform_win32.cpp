#include "app/platform/platform.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace idiff {

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

} // namespace idiff
