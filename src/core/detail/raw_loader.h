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
    std::unique_ptr<Image> load(const std::string& path);

    // Check if file extension looks like a RAW format.
    static bool is_raw_extension(const std::string& path);

    const std::string& last_error() const noexcept;

private:
    std::string last_error_;
};

} // namespace idiff

#endif // IDIFF_RAW_LOADER_H
