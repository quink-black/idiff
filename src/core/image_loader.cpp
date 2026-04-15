#include "core/image_loader.h"
#include "core/image_impl.h"

#include <algorithm>

#include <Magick++.h>
#include <opencv2/core.hpp>

#include "core/detail/raw_loader.h"

namespace idiff {

namespace {

const char* raw_extensions[] = {
    ".dng", ".cr2", ".nef", ".arw", ".rw2",
    ".orf", ".pef", ".sr2", ".srw", ".raw",
    ".raf", ".mrw", ".x3f", ".3fr", ".iiq",
    ".kdc", ".dcr", ".erf", ".mef", ".mos",
    ".nrw", ".ptx", ".r3d", ".rwz", ".srf",
};

SourceFormat extension_to_format(const std::string& path) {
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return SourceFormat::Unknown;

    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (ext == ".png")  return SourceFormat::PNG;
    if (ext == ".jpg" || ext == ".jpeg") return SourceFormat::JPEG;
    if (ext == ".webp") return SourceFormat::WebP;
    if (ext == ".tif" || ext == ".tiff") return SourceFormat::TIFF;
    if (ext == ".bmp")  return SourceFormat::BMP;

    for (const auto* raw_ext : raw_extensions) {
        if (ext == raw_ext) return SourceFormat::RAW;
    }

    return SourceFormat::Unknown;
}

PixelFormat magick_type_to_pixel_format(const Magick::Image& img) {
    bool is_gray = img.type() == Magick::GrayscaleType ||
                   img.type() == Magick::GrayscaleAlphaType;
    bool has_alpha = img.alpha();
    size_t depth = img.depth();

    if (is_gray) {
        return depth > 8 ? PixelFormat::Gray16 : PixelFormat::Gray8;
    }
    if (has_alpha) {
        return depth > 8 ? PixelFormat::RGBA16 : PixelFormat::RGBA8;
    }
    return depth > 8 ? PixelFormat::RGB16 : PixelFormat::RGB8;
}

int pixel_format_channels(PixelFormat fmt) {
    switch (fmt) {
        case PixelFormat::Gray8:
        case PixelFormat::Gray16:  return 1;
        case PixelFormat::RGB8:
        case PixelFormat::RGB16:   return 3;
        case PixelFormat::RGBA8:
        case PixelFormat::RGBA16:  return 4;
    }
    return 3;
}

int pixel_format_depth(PixelFormat fmt) {
    switch (fmt) {
        case PixelFormat::Gray16:
        case PixelFormat::RGB16:
        case PixelFormat::RGBA16:  return 16;
        default:                    return 8;
    }
}

bool has_flag(uint32_t flags, LoadFlag flag) {
    return (flags & static_cast<uint32_t>(flag)) != 0;
}

// Extract pixel data from a Magick::Image into the Image's cv::Mat and ImageInfo.
// The Magick::Image is used only during this call and is not retained.
void populate_image_from_magick(Magick::Image& mi, Image& image) {
    auto& impl = image.internal();

    impl.info.width = static_cast<int>(mi.columns());
    impl.info.height = static_cast<int>(mi.rows());
    impl.info.pixel_format = magick_type_to_pixel_format(mi);
    impl.info.bit_depth = pixel_format_depth(impl.info.pixel_format);
    impl.info.has_alpha = mi.alpha();

    // Check for ICC profile
    try {
        Magick::Blob icc_blob = mi.profile("icc");
        if (icc_blob.length() > 0) {
            impl.info.icc_profile_name = "Embedded ICC";
        }
    } catch (...) {
        // No ICC profile
    }

    impl.info.color_space = mi.colorSpace() == Magick::sRGBColorspace
        ? "sRGB" : "Unknown";

    // Export pixels to cv::Mat
    int channels = pixel_format_channels(impl.info.pixel_format);
    int cv_type = impl.info.bit_depth > 8
        ? CV_16UC(channels) : CV_8UC(channels);

    cv::Mat mat(impl.info.height, impl.info.width, cv_type);

    // Mapping string must match the actual pixel format:
    //   Grayscale → "I" (intensity)
    //   RGB       → "RGB"
    //   RGBA      → "RGBA"
    std::string mapping;
    if (channels == 1) {
        mapping = "I";
    } else if (channels == 4) {
        mapping = "RGBA";
    } else {
        mapping = "RGB";
    }
    auto storage = impl.info.bit_depth > 8 ? Magick::ShortPixel : Magick::CharPixel;

    mi.write(0, 0, mi.columns(), mi.rows(), mapping, storage, mat.ptr());
    impl.mat = std::move(mat);
}

} // namespace

ImageLoader::ImageLoader(uint32_t flags) : flags_(flags) {}

std::unique_ptr<Image> ImageLoader::load(const std::string& path) {
    last_error_.clear();

    if (is_raw_format(path)) {
        return load_via_raw(path);
    }
    return load_via_magick(path);
}

std::unique_ptr<Image> ImageLoader::load_from_memory(const uint8_t* data, size_t size,
                                                      SourceFormat format) {
    last_error_.clear();

    if (format == SourceFormat::RAW || is_raw_format(format)) {
        last_error_ = "RAW loading from memory is not supported";
        return nullptr;
    }

    try {
        Magick::Blob blob(data, size);
        Magick::Image magick_img(blob);

        if (!has_flag(flags_, LoadFlag::KeepAlpha)) {
            magick_img.type(Magick::TrueColorType);
        }

        auto image = std::make_unique<Image>();
        image->internal().info.source_format = format;
        populate_image_from_magick(magick_img, *image);

        return image;
    } catch (const Magick::Exception& e) {
        last_error_ = e.what();
        return nullptr;
    }
}

const std::string& ImageLoader::last_error() const noexcept {
    return last_error_;
}

std::unique_ptr<Image> ImageLoader::load_via_magick(const std::string& path) {
    try {
        Magick::Image magick_img(path);

        if (!has_flag(flags_, LoadFlag::KeepAlpha)) {
            magick_img.type(Magick::TrueColorType);
        }

        auto image = std::make_unique<Image>();
        image->internal().info.source_format = extension_to_format(path);
        populate_image_from_magick(magick_img, *image);

        return image;
    } catch (const Magick::Exception& e) {
        last_error_ = e.what();
        return nullptr;
    }
}

std::unique_ptr<Image> ImageLoader::load_via_raw(const std::string& path) {
    RawLoader raw_loader;
    auto image = raw_loader.load(path);
    if (!image) {
        last_error_ = raw_loader.last_error();
    }
    return image;
}

bool ImageLoader::is_raw_format(const std::string& path) {
    return RawLoader::is_raw_extension(path);
}

bool ImageLoader::is_raw_format(SourceFormat format) {
    return format == SourceFormat::RAW;
}

} // namespace idiff
