#ifndef IDIFF_RAW_LOADER_H
#define IDIFF_RAW_LOADER_H

#include <cstdint>
#include <memory>
#include <string>

#include "core/image.h"

namespace idiff {

class RawLoader {
public:
    // Decode RAW file to Image with cv::Mat pixels.
    // When keep_16bit is true, output preserves 16-bit depth from the sensor.
    std::unique_ptr<Image> load(const std::string& path, bool keep_16bit = false);

    // Check if file extension looks like a RAW format.
    static bool is_raw_extension(const std::string& path);

    const std::string& last_error() const noexcept;

private:
    std::string last_error_;
};

} // namespace idiff

#endif // IDIFF_RAW_LOADER_H
