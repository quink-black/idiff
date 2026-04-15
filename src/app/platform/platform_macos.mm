#include "app/platform/platform.h"

#include <AppKit/AppKit.h>

namespace idiff {

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

} // namespace idiff
