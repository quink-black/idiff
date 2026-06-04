#include "core/media_source.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <regex>
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
        case SourceFormat::HEIF: return "HEIF";
        case SourceFormat::AVIF: return "AVIF";
        default:                 return "Unknown";
    }
}

std::string make_format_desc(const ImageInfo& info) {
    std::string s = source_format_name(info.source_format);
    s += ' ';
    int display_depth = (info.source_bit_depth > 0) ? info.source_bit_depth
                                                     : info.bit_depth;
    s += std::to_string(display_depth);
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

std::string to_lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

// -------- ImageFileSource --------

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

    ImageLoader loader(static_cast<uint32_t>(LoadFlag::KeepAlpha) |
                       static_cast<uint32_t>(LoadFlag::ApplyICC));
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

// -------- YUV helpers --------

const char* yuv_pixel_format_name(YuvPixelFormat f) noexcept {
    switch (f) {
        case YuvPixelFormat::YUV420P:   return "YUV420P";
        case YuvPixelFormat::YUV422P:   return "YUV422P";
        case YuvPixelFormat::YUV444P:   return "YUV444P";
        case YuvPixelFormat::YUV420P10: return "YUV420P10";
        case YuvPixelFormat::YUV422P10: return "YUV422P10";
        case YuvPixelFormat::YUV444P10: return "YUV444P10";
        case YuvPixelFormat::P010:      return "P010";
        case YuvPixelFormat::NV16:      return "NV16";
    }
    return "YUV?";
}

const char* yuv_color_range_name(YuvColorRange r) noexcept {
    switch (r) {
        case YuvColorRange::Limited: return "Limited";
        case YuvColorRange::Full:    return "Full";
    }
    return "?";
}

const char* yuv_color_matrix_name(YuvColorMatrix m) noexcept {
    switch (m) {
        case YuvColorMatrix::BT601:      return "BT.601";
        case YuvColorMatrix::BT709:      return "BT.709";
        case YuvColorMatrix::BT2020_NCL: return "BT.2020 NCL";
    }
    return "?";
}

const char* yuv_color_primaries_name(YuvColorPrimaries p) noexcept {
    switch (p) {
        case YuvColorPrimaries::BT601:  return "BT.601";
        case YuvColorPrimaries::BT709:  return "BT.709";
        case YuvColorPrimaries::BT2020: return "BT.2020";
    }
    return "?";
}

int yuv_pixel_format_bit_depth(YuvPixelFormat f) noexcept {
    switch (f) {
        case YuvPixelFormat::YUV420P:
        case YuvPixelFormat::YUV422P:
        case YuvPixelFormat::YUV444P:
        case YuvPixelFormat::NV16:
            return 8;
        case YuvPixelFormat::YUV420P10:
        case YuvPixelFormat::YUV422P10:
        case YuvPixelFormat::YUV444P10:
        case YuvPixelFormat::P010:
            return 10;
    }
    return 0;
}

std::size_t yuv_frame_size_bytes(const YuvStreamParams& p) noexcept {
    if (p.width <= 0 || p.height <= 0) return 0;
    const auto W = static_cast<std::size_t>(p.width);
    const auto H = static_cast<std::size_t>(p.height);

    switch (p.pixel_format) {
        case YuvPixelFormat::YUV420P:
            if ((p.width & 1) || (p.height & 1)) return 0;
            return W * H * 3 / 2;
        case YuvPixelFormat::YUV422P:
            if (p.width & 1) return 0;
            return W * H * 2;
        case YuvPixelFormat::YUV444P:
            return W * H * 3;
        case YuvPixelFormat::YUV420P10:
            if ((p.width & 1) || (p.height & 1)) return 0;
            return W * H * 2 + (W / 2) * (H / 2) * 2 * 2;
        case YuvPixelFormat::YUV422P10:
            if (p.width & 1) return 0;
            return W * H * 2 + (W / 2) * H * 2 * 2;
        case YuvPixelFormat::YUV444P10:
            return W * H * 2 * 3;
        case YuvPixelFormat::P010:
            if ((p.width & 1) || (p.height & 1)) return 0;
            // Y: W*H pixels, 16-bit LE each; UV: (W/2)*(H/2) pairs, 16-bit LE each
            return W * H * 2 + (W / 2) * (H / 2) * 2 * 2;
        case YuvPixelFormat::NV16:
            if (p.width & 1) return 0;
            // Y: W*H bytes; UV interleaved: W*H bytes (W/2 pairs per row, 2 bytes each)
            return W * H + W * H;
    }
    return 0;
}

bool guess_yuv_params_from_filename(const std::string& path, YuvStreamParams& out) {
    // Work on just the basename so directory names can't pollute the
    // match.  Lowercased so regexes stay simple.
    std::string name;
    auto sep = path.find_last_of("/\\");
    name = (sep != std::string::npos) ? path.substr(sep + 1) : path;
    name = to_lower(name);

    bool changed = false;

    // Resolution:  WIDTHxHEIGHT anywhere in the name, e.g. 1920x1080.
    std::smatch m;
    std::regex re_res(R"((\d{2,5})x(\d{2,5}))");
    if (std::regex_search(name, m, re_res)) {
        int w = std::stoi(m[1].str());
        int h = std::stoi(m[2].str());
        if (w > 0 && h > 0 && w <= 16384 && h <= 16384) {
            out.width = w;
            out.height = h;
            changed = true;
        }
    }

    // Pixel format keywords.  Longer/more-specific strings must be
    // checked before shorter ones so "yuv420p10" is not swallowed by
    // "yuv420p".
    struct FmtKeyword {
        const char* key;
        YuvPixelFormat fmt;
    };
    const FmtKeyword kws[] = {
        {"yuv420p10", YuvPixelFormat::YUV420P10},
        {"yuv422p10", YuvPixelFormat::YUV422P10},
        {"yuv444p10", YuvPixelFormat::YUV444P10},
        {"p010le",    YuvPixelFormat::P010},
        {"p010",      YuvPixelFormat::P010},
        {"nv16",      YuvPixelFormat::NV16},
        {"yuv420p",   YuvPixelFormat::YUV420P},
        {"yuv422p",   YuvPixelFormat::YUV422P},
        {"yuv444p",   YuvPixelFormat::YUV444P},
        {"i420",      YuvPixelFormat::YUV420P},
    };
    for (const auto& kw : kws) {
        if (name.find(kw.key) != std::string::npos) {
            out.pixel_format = kw.fmt;
            changed = true;
            break;
        }
    }

    // Color range hints.
    if (name.find("fullrange") != std::string::npos ||
        name.find("full") != std::string::npos) {
        out.color_range = YuvColorRange::Full;
        changed = true;
    } else if (name.find("limited") != std::string::npos ||
               name.find("tv") != std::string::npos) {
        out.color_range = YuvColorRange::Limited;
        changed = true;
    }

    // Color matrix hints.  bt2020 must be checked before bt601/bt709
    // because "bt2020" contains neither "bt601" nor "bt709" as a
    // substring, but checking the longer one first is safer.
    if (name.find("bt2020") != std::string::npos) {
        out.color_matrix = YuvColorMatrix::BT2020_NCL;
        out.color_primaries = YuvColorPrimaries::BT2020;
        changed = true;
    } else if (name.find("bt709") != std::string::npos) {
        out.color_matrix = YuvColorMatrix::BT709;
        out.color_primaries = YuvColorPrimaries::BT709;
        changed = true;
    } else if (name.find("bt601") != std::string::npos ||
               name.find("smpte170m") != std::string::npos) {
        out.color_matrix = YuvColorMatrix::BT601;
        out.color_primaries = YuvColorPrimaries::BT601;
        changed = true;
    }

    return changed;
}

} // namespace idiff
