#include "core/image_loader.h"
#include "core/image_impl.h"

#include <algorithm>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

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

PixelFormat mat_to_pixel_format(const cv::Mat& mat) {
    int channels = mat.channels();
    int depth = mat.depth();
    bool is_16bit = (depth == CV_16U || depth == CV_16S);

    if (channels == 1) {
        return is_16bit ? PixelFormat::Gray16 : PixelFormat::Gray8;
    }
    if (channels == 4) {
        return is_16bit ? PixelFormat::RGBA16 : PixelFormat::RGBA8;
    }
    return is_16bit ? PixelFormat::RGB16 : PixelFormat::RGB8;
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

void populate_image_from_mat(cv::Mat& mat, Image& image, bool keep_alpha) {
    auto& impl = image.internal();

    // OpenCV decodes as BGR; convert to RGB for our internal representation.
    if (mat.channels() == 3) {
        cv::cvtColor(mat, mat, cv::COLOR_BGR2RGB);
    } else if (mat.channels() == 4) {
        cv::cvtColor(mat, mat, cv::COLOR_BGRA2RGBA);
    }

    if (!keep_alpha && mat.channels() == 4) {
        cv::Mat rgb;
        cv::cvtColor(mat, rgb, cv::COLOR_RGBA2RGB);
        mat = rgb;
    }

    impl.info.width = mat.cols;
    impl.info.height = mat.rows;
    impl.info.pixel_format = mat_to_pixel_format(mat);
    impl.info.bit_depth = pixel_format_depth(impl.info.pixel_format);
    impl.info.has_alpha = (mat.channels() == 4);
    impl.info.color_space = "sRGB";

    impl.mat = std::move(mat);
}

} // namespace

ImageLoader::ImageLoader(uint32_t flags) : flags_(flags) {}

std::unique_ptr<Image> ImageLoader::load(const std::string& path) {
    last_error_.clear();

    if (is_raw_format(path)) {
        return load_via_raw(path);
    }
    return load_via_opencv(path);
}

std::unique_ptr<Image> ImageLoader::load_from_memory(const uint8_t* data, size_t size,
                                                      SourceFormat format) {
    last_error_.clear();

    if (format == SourceFormat::RAW || is_raw_format(format)) {
        last_error_ = "RAW loading from memory is not supported";
        return nullptr;
    }

    try {
        std::vector<uint8_t> buf(data, data + size);
        int imread_flags = cv::IMREAD_UNCHANGED;
        if (!has_flag(flags_, LoadFlag::Keep16Bit)) {
            imread_flags = has_flag(flags_, LoadFlag::KeepAlpha)
                ? cv::IMREAD_UNCHANGED : cv::IMREAD_COLOR;
        }

        cv::Mat mat = cv::imdecode(buf, imread_flags);
        if (mat.empty()) {
            last_error_ = "Failed to decode image from memory";
            return nullptr;
        }

        auto image = std::make_unique<Image>();
        image->internal().info.source_format = format;
        populate_image_from_mat(mat, *image, has_flag(flags_, LoadFlag::KeepAlpha));

        return image;
    } catch (const cv::Exception& e) {
        last_error_ = e.what();
        return nullptr;
    }
}

const std::string& ImageLoader::last_error() const noexcept {
    return last_error_;
}

std::unique_ptr<Image> ImageLoader::load_via_opencv(const std::string& path) {
    try {
        int imread_flags = cv::IMREAD_UNCHANGED;
        if (!has_flag(flags_, LoadFlag::Keep16Bit)) {
            imread_flags = has_flag(flags_, LoadFlag::KeepAlpha)
                ? cv::IMREAD_UNCHANGED : cv::IMREAD_COLOR;
        }

        cv::Mat mat = cv::imread(path, imread_flags);
        if (mat.empty()) {
            last_error_ = "Failed to load image: " + path;
            return nullptr;
        }

        auto image = std::make_unique<Image>();
        image->internal().info.source_format = extension_to_format(path);
        populate_image_from_mat(mat, *image, has_flag(flags_, LoadFlag::KeepAlpha));

        return image;
    } catch (const cv::Exception& e) {
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
