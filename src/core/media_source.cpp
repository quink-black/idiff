#include "core/media_source.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <regex>
#include <utility>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

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

// -------- YUV helpers --------

const char* yuv_pixel_format_name(YuvPixelFormat f) noexcept {
    switch (f) {
        case YuvPixelFormat::YUV420P: return "YUV420P";
        case YuvPixelFormat::YUV422P: return "YUV422P";
        case YuvPixelFormat::YUV444P: return "YUV444P";
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

std::size_t yuv_frame_size_bytes(const YuvStreamParams& p) noexcept {
    if (p.width <= 0 || p.height <= 0) return 0;
    // YUV 420P and 422P require even width/height for chroma subsampling.
    switch (p.pixel_format) {
        case YuvPixelFormat::YUV420P:
            if ((p.width & 1) || (p.height & 1)) return 0;
            return static_cast<std::size_t>(p.width) * p.height * 3 / 2;
        case YuvPixelFormat::YUV422P:
            if (p.width & 1) return 0;
            return static_cast<std::size_t>(p.width) * p.height * 2;
        case YuvPixelFormat::YUV444P:
            return static_cast<std::size_t>(p.width) * p.height * 3;
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

    // Pixel format keywords.  Longer strings (e.g. "yuv420p") must be
    // checked before shorter ones (e.g. "yuv420") so i420 and yuv420p
    // don't accidentally match "yuv422p".
    struct FmtKeyword {
        const char* key;
        YuvPixelFormat fmt;
    };
    const FmtKeyword kws[] = {
        {"yuv420p", YuvPixelFormat::YUV420P},
        {"yuv422p", YuvPixelFormat::YUV422P},
        {"yuv444p", YuvPixelFormat::YUV444P},
        {"i420",    YuvPixelFormat::YUV420P},
    };
    for (const auto& kw : kws) {
        if (name.find(kw.key) != std::string::npos) {
            out.pixel_format = kw.fmt;
            changed = true;
            break;
        }
    }

    // Color range hints.
    if (name.find("full") != std::string::npos ||
        name.find("fullrange") != std::string::npos) {
        out.color_range = YuvColorRange::Full;
        changed = true;
    } else if (name.find("limited") != std::string::npos ||
               name.find("tv") != std::string::npos) {
        out.color_range = YuvColorRange::Limited;
        changed = true;
    }

    return changed;
}

// -------- YuvRawSource --------

namespace {

// Compute YUV plane sizes.  Returns {y_bytes, u_bytes, v_bytes}.
struct PlaneSizes { int y_w, y_h, uv_w, uv_h; };

PlaneSizes plane_sizes(const YuvStreamParams& p) {
    PlaneSizes s{};
    s.y_w = p.width;
    s.y_h = p.height;
    switch (p.pixel_format) {
        case YuvPixelFormat::YUV420P:
            s.uv_w = p.width / 2; s.uv_h = p.height / 2; break;
        case YuvPixelFormat::YUV422P:
            s.uv_w = p.width / 2; s.uv_h = p.height;     break;
        case YuvPixelFormat::YUV444P:
            s.uv_w = p.width;     s.uv_h = p.height;     break;
    }
    return s;
}

// Decode one planar YUV frame (Y/U/V as separate byte pointers) into an
// RGB8 cv::Mat using BT.601 matrix.  Handles limited vs full range by
// rescaling the planes before the standard cvtColor() conversion, which
// assumes BT.601 limited range.  The output is RGB (not BGR) to match
// the rest of idiff's in-memory convention.
cv::Mat decode_planar_yuv_to_rgb(const uint8_t* y, const uint8_t* u, const uint8_t* v,
                                 const YuvStreamParams& p,
                                 const PlaneSizes& ps) {
    cv::Mat y_mat(ps.y_h, ps.y_w, CV_8UC1, const_cast<uint8_t*>(y));
    cv::Mat u_mat(ps.uv_h, ps.uv_w, CV_8UC1, const_cast<uint8_t*>(u));
    cv::Mat v_mat(ps.uv_h, ps.uv_w, CV_8UC1, const_cast<uint8_t*>(v));

    // Copy since the source pointers live in a temporary byte buffer
    // that will go out of scope after read_frame() returns.
    y_mat = y_mat.clone();
    u_mat = u_mat.clone();
    v_mat = v_mat.clone();

    // Upsample U/V to full resolution so we can build a 3-channel YUV
    // mat that cvtColor accepts.  Nearest neighbor is fine for 4:2:2
    // and 4:2:0 here since displayed pixels go through the normal
    // scaling path later anyway.
    if (ps.uv_w != ps.y_w || ps.uv_h != ps.y_h) {
        cv::resize(u_mat, u_mat, cv::Size(ps.y_w, ps.y_h), 0, 0, cv::INTER_LINEAR);
        cv::resize(v_mat, v_mat, cv::Size(ps.y_w, ps.y_h), 0, 0, cv::INTER_LINEAR);
    }

    // For Full-range sources, contract Y [0,255] -> [16,235] and UV
    // [0,255] -> [16,240] so that the subsequent limited-range cvtColor
    // produces correct output.  This is a simple per-pixel affine
    // transform done in-place.
    if (p.color_range == YuvColorRange::Full) {
        y_mat.convertTo(y_mat, CV_8U, 219.0 / 255.0, 16.0);
        u_mat.convertTo(u_mat, CV_8U, 224.0 / 255.0, 16.0);
        v_mat.convertTo(v_mat, CV_8U, 224.0 / 255.0, 16.0);
    }

    std::vector<cv::Mat> planes = { y_mat, u_mat, v_mat };
    cv::Mat yuv;
    cv::merge(planes, yuv);

    cv::Mat rgb;
    // OpenCV's YUV2RGB expects BT.601 limited range.  We've already
    // compensated above for full-range sources.
    cv::cvtColor(yuv, rgb, cv::COLOR_YUV2RGB);
    return rgb;
}

} // namespace

YuvRawSource::YuvRawSource(std::string path, const YuvStreamParams& params)
    : path_(std::move(path)), params_(params) {
    frame_bytes_ = yuv_frame_size_bytes(params_);
    frame_count_ = 0;
    if (frame_bytes_ > 0) {
        std::error_code ec;
        auto size = std::filesystem::file_size(path_, ec);
        if (!ec && size >= frame_bytes_) {
            frame_count_ = static_cast<int>(size / frame_bytes_);
        }
    }

    format_desc_ = std::string(yuv_pixel_format_name(params_.pixel_format)) + " "
                 + std::to_string(params_.width) + "x"
                 + std::to_string(params_.height) + " 8-bit "
                 + yuv_color_range_name(params_.color_range);
}

YuvRawSource::~YuvRawSource() = default;

std::unique_ptr<Image> YuvRawSource::read_frame(int index) {
    if (frame_bytes_ == 0) {
        last_error_ = "invalid YUV parameters";
        return nullptr;
    }
    if (index < 0 || index >= frame_count_) {
        last_error_ = "frame index out of range";
        return nullptr;
    }

    // Open and seek to the requested frame.  We re-open for every read
    // to keep the source stateless; a persistent FILE* can be added later
    // if sequential scrubbing turns out to be a hotspot.
    std::FILE* fp = std::fopen(path_.c_str(), "rb");
    if (!fp) {
        last_error_ = std::string("cannot open YUV file: ") + std::strerror(errno);
        return nullptr;
    }

    const std::size_t offset = static_cast<std::size_t>(index) * frame_bytes_;
    if (std::fseek(fp, static_cast<long>(offset), SEEK_SET) != 0) {
        std::fclose(fp);
        last_error_ = "seek failed";
        return nullptr;
    }

    std::vector<uint8_t> buf(frame_bytes_);
    std::size_t got = std::fread(buf.data(), 1, frame_bytes_, fp);
    std::fclose(fp);
    if (got != frame_bytes_) {
        last_error_ = "short read";
        return nullptr;
    }

    PlaneSizes ps = plane_sizes(params_);
    const std::size_t y_bytes = static_cast<std::size_t>(ps.y_w) * ps.y_h;
    const std::size_t uv_bytes = static_cast<std::size_t>(ps.uv_w) * ps.uv_h;
    const uint8_t* y = buf.data();
    const uint8_t* u = y + y_bytes;
    const uint8_t* v = u + uv_bytes;

    cv::Mat rgb = decode_planar_yuv_to_rgb(y, u, v, params_, ps);
    if (rgb.empty()) {
        last_error_ = "decode failed";
        return nullptr;
    }

    auto img = std::make_unique<Image>();
    img->internal().mat = rgb;
    img->internal().info.width = rgb.cols;
    img->internal().info.height = rgb.rows;
    img->internal().info.pixel_format = PixelFormat::RGB8;
    img->internal().info.source_format = SourceFormat::Unknown;
    img->internal().info.bit_depth = 8;
    img->internal().info.has_alpha = false;
    img->internal().info.color_space = std::string("BT.601 ")
        + yuv_color_range_name(params_.color_range);

    last_error_.clear();
    return img;
}

} // namespace idiff
