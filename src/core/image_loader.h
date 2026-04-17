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

// Which decoder library to use for general (non-RAW) images.  Both
// backends may be compiled into the same binary; ImageMagick is preferred
// when available because it handles ICC profiles and a wider set of
// formats, while OpenCV imgcodecs is always present and serves as the
// fallback.
enum class LoaderBackend {
    ImageMagick,  // Magick++; only available when built with IDIFF_HAVE_MAGICK
    OpenCV,       // opencv_imgcodecs; always available
};

class ImageLoader {
public:
    explicit ImageLoader(uint32_t flags = static_cast<uint32_t>(LoadFlag::ApplyICC));

    // Default backend is the "best" one compiled in: ImageMagick when
    // available, otherwise OpenCV.  load() still transparently falls back
    // to the other backend if the preferred one fails.
    void set_preferred_backend(LoaderBackend backend) noexcept;
    LoaderBackend preferred_backend() const noexcept;

    // Which backend actually decoded the most recent successful load().
    // Undefined before any successful load.
    LoaderBackend last_used_backend() const noexcept;

    std::unique_ptr<Image> load(const std::string& path);
    std::unique_ptr<Image> load_from_memory(const uint8_t* data, size_t size,
                                            SourceFormat format = SourceFormat::Unknown);

    const std::string& last_error() const noexcept;

    // Compile-time capability queries -- use these to decide which
    // backend choices to show in the UI.
    static bool has_backend(LoaderBackend backend) noexcept;
    static LoaderBackend default_backend() noexcept;
    static const char* backend_name(LoaderBackend backend) noexcept;

private:
    uint32_t flags_;
    std::string last_error_;
    LoaderBackend preferred_ = default_backend();
    LoaderBackend last_used_ = default_backend();

    std::unique_ptr<Image> load_with_backend(const std::string& path,
                                             LoaderBackend backend);
    std::unique_ptr<Image> load_from_memory_with_backend(const uint8_t* data,
                                                         size_t size,
                                                         SourceFormat format,
                                                         LoaderBackend backend);

    std::unique_ptr<Image> load_via_opencv(const std::string& path);
    std::unique_ptr<Image> load_via_opencv_memory(const uint8_t* data, size_t size,
                                                  SourceFormat format);
#ifdef IDIFF_HAVE_MAGICK
    std::unique_ptr<Image> load_via_magick(const std::string& path);
    std::unique_ptr<Image> load_via_magick_memory(const uint8_t* data, size_t size,
                                                  SourceFormat format);
#endif
    std::unique_ptr<Image> load_via_raw(const std::string& path);

    static bool is_raw_format(const std::string& path);
    static bool is_raw_format(SourceFormat format);
};

} // namespace idiff

#endif // IDIFF_IMAGE_LOADER_H
