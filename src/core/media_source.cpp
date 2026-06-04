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

bool guess_yuv_params_from_filename(const std::string& path, YuvStreamParams& out) {
    std::string name;
    auto sep = path.find_last_of("/\\");
    name = (sep != std::string::npos) ? path.substr(sep + 1) : path;
    name = to_lower(name);

    bool changed = false;

    // Resolution: WIDTHxHEIGHT anywhere in the name.
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

    // Pixel format keywords.  Longer/more-specific strings first so
    // "yuv420p10le" is not swallowed by "yuv420p10" or "yuv420p".
    // These map directly to FFmpeg pixel format names.
    struct FmtKeyword {
        const char* key;
        const char* fmt;
    };
    const FmtKeyword kws[] = {
        {"yuv420p10le", "yuv420p10le"},
        {"yuv422p10le", "yuv422p10le"},
        {"yuv444p10le", "yuv444p10le"},
        {"yuv420p10",   "yuv420p10le"},
        {"yuv422p10",   "yuv422p10le"},
        {"yuv444p10",   "yuv444p10le"},
        {"p010le",      "p010le"},
        {"p010",        "p010le"},
        {"nv12",        "nv12"},
        {"yuv420p",     "yuv420p"},
        {"yuv422p",     "yuv422p"},
        {"yuv444p",     "yuv444p"},
        {"i420",        "yuv420p"},
    };
    for (const auto& kw : kws) {
        if (name.find(kw.key) != std::string::npos) {
            out.pixel_format = kw.fmt;
            changed = true;
            break;
        }
    }

    // Color metadata (range, matrix, primaries, transfer) is left
    // untouched on purpose.  Filename-based heuristics for those tags
    // are too unreliable -- substrings like "bt709" or "pq" routinely
    // appear in unrelated filename fragments -- and silently picking
    // the wrong colour space produces visually plausible but
    // numerically wrong frames that are hard to diagnose.  The user
    // configures these explicitly in the dialog instead.

    return changed;
}

} // namespace idiff
