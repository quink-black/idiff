#ifndef IDIFF_IMAGE_H
#define IDIFF_IMAGE_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace cv { class Mat; }

namespace idiff {

enum class PixelFormat {
    Gray8,
    Gray16,
    RGB8,
    RGB16,
    RGBA8,
    RGBA16,
};

enum class SourceFormat {
    PNG,
    JPEG,
    WebP,
    TIFF,
    BMP,
    RAW,
    HEIF,
    AVIF,
    Unknown,
};

struct ImageInfo {
    int width = 0;
    int height = 0;
    PixelFormat pixel_format = PixelFormat::RGB8;
    SourceFormat source_format = SourceFormat::Unknown;
    int bit_depth = 8;
    int source_bit_depth = 0;  // original file depth; 0 = same as bit_depth
    std::string icc_profile_name;
    std::string color_space;
    bool has_alpha = false;

    // Sample aspect ratio (SAR).  When both are non-zero, display
    // dimensions are width * sar_num / sar_den x height.
    // A value of (0, 0) means unspecified (treated as 1:1).
    int sar_num = 0;
    int sar_den = 0;

    // Display dimensions: pixel size adjusted by SAR.
    // Equal to width/height when SAR is 1:1 or unset.
    int display_width() const noexcept;
    int display_height() const noexcept;
};

class Image {
public:
    Image();
    ~Image();

    Image(Image&&) noexcept;
    Image& operator=(Image&&) noexcept;

    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    const ImageInfo& info() const noexcept;
    const uint8_t* pixels() const noexcept;
    std::size_t resident_bytes() const noexcept;

    const cv::Mat& mat() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

public:
    // Internal access for core library components.
    // Not part of the stable public API.
    Impl& internal() noexcept { return *impl_; }
    const Impl& internal() const noexcept { return *impl_; }
};

} // namespace idiff

#endif // IDIFF_IMAGE_H
