#include "core/media_source.h"

#include <utility>

#include "core/image.h"
#include "core/image_impl.h"

namespace idiff {

namespace {

const char* source_format_name(SourceFormat f) noexcept {
    switch (f) {
        case SourceFormat::PNG:  return "PNG";
        case SourceFormat::JPEG: return "JPEG";
        case SourceFormat::WebP: return "WebP";
        case SourceFormat::TIFF: return "TIFF";
        case SourceFormat::BMP:  return "BMP";
        case SourceFormat::RAW:  return "RAW";
        default:                 return "Unknown";
    }
}

std::string make_format_desc(const ImageInfo& info) {
    std::string s = source_format_name(info.source_format);
    s += ' ';
    s += std::to_string(info.bit_depth);
    s += "-bit ";
    if (info.has_alpha) {
        s += "RGBA";
    } else {
        switch (info.pixel_format) {
            case PixelFormat::Gray8:
            case PixelFormat::Gray16:
                s += "Gray"; break;
            default:
                s += "RGB"; break;
        }
    }
    return s;
}

} // namespace

ImageFileSource::ImageFileSource(std::string path, LoaderBackend preferred_backend)
    : path_(std::move(path)), preferred_backend_(preferred_backend) {}

ImageFileSource::~ImageFileSource() = default;

void ImageFileSource::set_preferred_backend(LoaderBackend backend) noexcept {
    preferred_backend_ = backend;
}

std::unique_ptr<Image> ImageFileSource::read_frame(int index) {
    if (index != 0) {
        last_error_ = "frame index out of range";
        return nullptr;
    }

    ImageLoader loader;
    loader.set_preferred_backend(preferred_backend_);
    auto img = loader.load(path_);
    if (!img) {
        last_error_ = loader.last_error();
        return nullptr;
    }

    const auto& info = img->info();
    width_ = info.width;
    height_ = info.height;
    format_desc_ = make_format_desc(info);
    last_error_.clear();
    return img;
}

} // namespace idiff
