#ifndef IDIFF_IMAGE_LOADER_H
#define IDIFF_IMAGE_LOADER_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "core/image.h"

namespace idiff {

enum class LoadFlag : uint32_t {
    None      = 0,
    Keep16Bit = 1 << 0,
    KeepAlpha = 1 << 1,
    ApplyICC  = 1 << 2,
};

inline LoadFlag operator|(LoadFlag a, LoadFlag b) {
    return static_cast<LoadFlag>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline bool operator&(LoadFlag a, LoadFlag b) {
    return (static_cast<uint32_t>(a) & static_cast<uint32_t>(b)) != 0;
}

class ImageLoader {
public:
    explicit ImageLoader(uint32_t flags = static_cast<uint32_t>(LoadFlag::ApplyICC));

    std::unique_ptr<Image> load(const std::string& path);
    std::unique_ptr<Image> load_from_memory(const uint8_t* data, size_t size,
                                            SourceFormat format = SourceFormat::Unknown);

    const std::string& last_error() const noexcept;

private:
    uint32_t flags_;
    std::string last_error_;

    std::unique_ptr<Image> load_via_magick(const std::string& path);
    std::unique_ptr<Image> load_via_raw(const std::string& path);

    static bool is_raw_format(const std::string& path);
    static bool is_raw_format(SourceFormat format);
};

} // namespace idiff

#endif // IDIFF_IMAGE_LOADER_H
