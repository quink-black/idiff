#include "core/image_loader.h"
#include "core/image_impl.h"
#include "core/detail/platform_utf8.h"

#include <algorithm>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#ifdef IDIFF_HAVE_MAGICK
#include <Magick++.h>
#endif

#include "core/detail/raw_loader.h"

#ifdef IDIFF_HAVE_FFMPEG_IMAGE_DECODE
#include "core/detail/ffmpeg_image_loader.h"
#endif

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
    if (ext == ".heic" || ext == ".heif" || ext == ".hif")
        return SourceFormat::HEIF;
    if (ext == ".avif") return SourceFormat::AVIF;

    for (const auto* raw_ext : raw_extensions) {
        if (ext == raw_ext) return SourceFormat::RAW;
    }

    return SourceFormat::Unknown;
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

// ---- OpenCV imgcodecs backend ----
// Lacks ICC profile handling and color-space awareness but is always
// available and handles the common formats fine.

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

void populate_image_from_mat(cv::Mat& mat, Image& image, bool keep_alpha) {
    auto& impl = image.internal();

    // OpenCV decodes as BGR; our internal representation uses RGB.
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

#ifdef IDIFF_HAVE_MAGICK

// ---- ImageMagick (Magick++) backend ----
// Full ICC profile support, wide format coverage, color-space awareness.

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

// Extract pixel data from a Magick::Image into the Image's cv::Mat and ImageInfo.
// The Magick::Image is used only during this call and is not retained.
void populate_image_from_magick(Magick::Image& mi, Image& image) {
    auto& impl = image.internal();

    impl.info.width = static_cast<int>(mi.columns());
    impl.info.height = static_cast<int>(mi.rows());
    impl.info.pixel_format = magick_type_to_pixel_format(mi);
    impl.info.bit_depth = pixel_format_depth(impl.info.pixel_format);
    impl.info.source_bit_depth = static_cast<int>(mi.depth());
    impl.info.has_alpha = mi.alpha();

    try {
        Magick::Blob icc_blob = mi.profile("icc");
        if (icc_blob.length() > 0) {
            impl.info.icc_profile_name = "Embedded ICC";
        }
    } catch (...) {
        // No ICC profile embedded in this image.
    }

    impl.info.color_space = mi.colorSpace() == Magick::sRGBColorspace
        ? "sRGB" : "Unknown";

    int channels = pixel_format_channels(impl.info.pixel_format);
    int cv_type = impl.info.bit_depth > 8
        ? CV_16UC(channels) : CV_8UC(channels);

    cv::Mat mat(impl.info.height, impl.info.width, cv_type);

    // Magick++ pixel-export mapping string:
    //   Grayscale -> "I" (intensity), RGB -> "RGB", RGBA -> "RGBA"
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

#endif // IDIFF_HAVE_MAGICK

} // namespace

// ---- Static capability queries ----

bool ImageLoader::has_backend(LoaderBackend backend) noexcept {
    switch (backend) {
        case LoaderBackend::OpenCV:
            return true;
        case LoaderBackend::ImageMagick:
#ifdef IDIFF_HAVE_MAGICK
            return true;
#else
            return false;
#endif
        case LoaderBackend::FFmpeg:
#ifdef IDIFF_HAVE_FFMPEG_IMAGE_DECODE
            return true;
#else
            return false;
#endif
    }
    return false;
}

LoaderBackend ImageLoader::default_backend() noexcept {
#ifdef IDIFF_HAVE_MAGICK
    return LoaderBackend::ImageMagick;
#else
    return LoaderBackend::OpenCV;
#endif
}

const char* ImageLoader::backend_name(LoaderBackend backend) noexcept {
    switch (backend) {
        case LoaderBackend::ImageMagick: return "ImageMagick";
        case LoaderBackend::OpenCV:      return "OpenCV";
        case LoaderBackend::FFmpeg:      return "FFmpeg";
    }
    return "Unknown";
}

// ---- ImageLoader ----

ImageLoader::ImageLoader(uint32_t flags) : flags_(flags) {}

void ImageLoader::set_preferred_backend(LoaderBackend backend) noexcept {
    // Silently snap to a compiled-in backend -- callers that care can
    // gate their UI on has_backend() first.
    preferred_ = has_backend(backend) ? backend : default_backend();
}

LoaderBackend ImageLoader::preferred_backend() const noexcept {
    return preferred_;
}

LoaderBackend ImageLoader::last_used_backend() const noexcept {
    return last_used_;
}

std::unique_ptr<Image> ImageLoader::load(const std::string& path) {
    last_error_.clear();

    if (is_raw_format(path)) {
        auto img = load_via_raw(path);
        // RAW decoding is independent of the chosen backend; report
        // last_used_ as the current preference for transparency.
        last_used_ = preferred_;
        return img;
    }

    // Build the backend priority list.  HEIF/AVIF prefers the FFmpeg
    // backend (libav* is far more reliable than ImageMagick HEIF
    // delegates on Windows/vcpkg); everything else keeps the historic
    // ImageMagick -> OpenCV order honoured by `preferred_`.
    LoaderBackend candidates[3];
    int nb = 0;
    if (is_heif_family(path)) {
        candidates[nb++] = LoaderBackend::FFmpeg;
        candidates[nb++] = LoaderBackend::ImageMagick;
        candidates[nb++] = LoaderBackend::OpenCV;
    } else {
        LoaderBackend primary = preferred_;
        LoaderBackend secondary = (primary == LoaderBackend::ImageMagick)
            ? LoaderBackend::OpenCV : LoaderBackend::ImageMagick;
        candidates[nb++] = primary;
        candidates[nb++] = secondary;
    }

    // Try each compiled-in backend in order; collect every error so
    // the caller sees which backends were attempted.
    std::string combined;
    int tried = 0;
    for (int i = 0; i < nb; ++i) {
        LoaderBackend b = candidates[i];
        if (!has_backend(b)) continue;
        last_error_.clear();
        auto img = load_with_backend(path, b);
        if (img) {
            last_used_ = b;
            return img;
        }
        if (!combined.empty()) combined += "; ";
        combined += backend_name(b);
        combined += ": ";
        combined += last_error_;
        ++tried;
    }

    if (tried == 0) {
        last_error_ = "No image loader backend is available";
    } else {
        last_error_ = std::move(combined);
    }
    return nullptr;
}

std::unique_ptr<Image> ImageLoader::load_from_memory(const uint8_t* data, size_t size,
                                                      SourceFormat format) {
    last_error_.clear();

    if (format == SourceFormat::RAW || is_raw_format(format)) {
        last_error_ = "RAW loading from memory is not supported";
        return nullptr;
    }

    LoaderBackend candidates[3];
    int nb = 0;
    if (is_heif_family(format)) {
        candidates[nb++] = LoaderBackend::FFmpeg;
        candidates[nb++] = LoaderBackend::ImageMagick;
        candidates[nb++] = LoaderBackend::OpenCV;
    } else {
        LoaderBackend primary = preferred_;
        LoaderBackend secondary = (primary == LoaderBackend::ImageMagick)
            ? LoaderBackend::OpenCV : LoaderBackend::ImageMagick;
        candidates[nb++] = primary;
        candidates[nb++] = secondary;
    }

    std::string combined;
    int tried = 0;
    for (int i = 0; i < nb; ++i) {
        LoaderBackend b = candidates[i];
        if (!has_backend(b)) continue;
        last_error_.clear();
        auto img = load_from_memory_with_backend(data, size, format, b);
        if (img) {
            last_used_ = b;
            return img;
        }
        if (!combined.empty()) combined += "; ";
        combined += backend_name(b);
        combined += ": ";
        combined += last_error_;
        ++tried;
    }

    if (tried == 0) {
        last_error_ = "No image loader backend is available";
    } else {
        last_error_ = std::move(combined);
    }
    return nullptr;
}

std::unique_ptr<Image>
ImageLoader::load_with_backend(const std::string& path, LoaderBackend backend) {
    switch (backend) {
        case LoaderBackend::OpenCV:
            return load_via_opencv(path);
        case LoaderBackend::ImageMagick:
#ifdef IDIFF_HAVE_MAGICK
            return load_via_magick(path);
#else
            last_error_ = "ImageMagick backend not compiled in";
            return nullptr;
#endif
        case LoaderBackend::FFmpeg:
            return load_via_ffmpeg(path);
    }
    return nullptr;
}

std::unique_ptr<Image>
ImageLoader::load_from_memory_with_backend(const uint8_t* data, size_t size,
                                            SourceFormat format,
                                            LoaderBackend backend) {
    switch (backend) {
        case LoaderBackend::OpenCV:
            return load_via_opencv_memory(data, size, format);
        case LoaderBackend::ImageMagick:
#ifdef IDIFF_HAVE_MAGICK
            return load_via_magick_memory(data, size, format);
#else
            last_error_ = "ImageMagick backend not compiled in";
            return nullptr;
#endif
        case LoaderBackend::FFmpeg:
            return load_via_ffmpeg_memory(data, size, format);
    }
    return nullptr;
}

const std::string& ImageLoader::last_error() const noexcept {
    return last_error_;
}

// ---- OpenCV path / memory ----

std::unique_ptr<Image> ImageLoader::load_via_opencv(const std::string& path) {
    try {
        int imread_flags = cv::IMREAD_UNCHANGED;
        if (!has_flag(flags_, LoadFlag::Keep16Bit)) {
            imread_flags = has_flag(flags_, LoadFlag::KeepAlpha)
                ? cv::IMREAD_UNCHANGED : cv::IMREAD_COLOR;
        }

        cv::Mat mat;
        {
            auto buf = platform::read_file_binary(path);
            if (buf.empty()) {
                last_error_ = "Failed to read file: " + path;
                return nullptr;
            }
            mat = cv::imdecode(buf, imread_flags);
        }

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
    } catch (const std::exception& e) {
        // Catch std::bad_alloc and anything else that escapes the I/O
        // helpers so a broken file can never crash the process.
        last_error_ = std::string("Failed to load image: ") + e.what();
        return nullptr;
    }
}

std::unique_ptr<Image>
ImageLoader::load_via_opencv_memory(const uint8_t* data, size_t size,
                                     SourceFormat format) {
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
    } catch (const std::exception& e) {
        last_error_ = std::string("Failed to decode image: ") + e.what();
        return nullptr;
    }
}

// ---- ImageMagick path / memory ----

#ifdef IDIFF_HAVE_MAGICK

std::unique_ptr<Image> ImageLoader::load_via_magick(const std::string& path) {
    try {
        Magick::Blob blob;
        {
            auto buf = platform::read_file_binary(path);
            if (buf.empty()) {
                last_error_ = "Failed to read file: " + path;
                return nullptr;
            }
            blob = Magick::Blob(buf.data(), buf.size());
        }
        Magick::Image magick_img(blob);

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

std::unique_ptr<Image>
ImageLoader::load_via_magick_memory(const uint8_t* data, size_t size,
                                     SourceFormat format) {
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

#endif // IDIFF_HAVE_MAGICK

std::unique_ptr<Image> ImageLoader::load_via_raw(const std::string& path) {
    RawLoader raw_loader;
    auto image = raw_loader.load(path, has_flag(flags_, LoadFlag::Keep16Bit));
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

bool ImageLoader::is_heif_family(const std::string& path) {
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".heic" || ext == ".heif" || ext == ".hif" || ext == ".avif";
}

bool ImageLoader::is_heif_family(SourceFormat format) {
    return format == SourceFormat::HEIF || format == SourceFormat::AVIF;
}

// ---- FFmpeg path / memory ----
//
// The actual decoder lives in detail/ffmpeg_image_loader.* and is
// pulled in only when IDIFF_HAVE_FFMPEG_IMAGE_DECODE is on.  The stub
// path keeps the binary linkable (and produces a clear error) for
// builds without FFmpeg.

#ifdef IDIFF_HAVE_FFMPEG_IMAGE_DECODE

std::unique_ptr<Image> ImageLoader::load_via_ffmpeg(const std::string& path) {
    try {
        auto buf = platform::read_file_binary(path);
        if (buf.empty()) {
            last_error_ = "Failed to read file: " + path;
            return nullptr;
        }
        SourceFormat fmt = extension_to_format(path);
        auto image = load_heif_avif_from_memory(
            buf.data(), buf.size(), fmt, flags_, last_error_);
        if (image) {
            // load_heif_avif_from_memory does not know the on-disk
            // path, so it falls back to whatever caller-supplied
            // SourceFormat we passed -- but if extension_to_format
            // returned Unknown we still want the field set to the
            // detected format so the UI shows a useful label.
            if (image->internal().info.source_format == SourceFormat::Unknown) {
                image->internal().info.source_format = fmt;
            }
        }
        return image;
    } catch (const std::exception& e) {
        last_error_ = std::string("FFmpeg: ") + e.what();
        return nullptr;
    }
}

std::unique_ptr<Image>
ImageLoader::load_via_ffmpeg_memory(const uint8_t* data, size_t size,
                                     SourceFormat format) {
    try {
        return load_heif_avif_from_memory(
            data, size, format, flags_, last_error_);
    } catch (const std::exception& e) {
        last_error_ = std::string("FFmpeg: ") + e.what();
        return nullptr;
    }
}

#else

std::unique_ptr<Image> ImageLoader::load_via_ffmpeg(const std::string& /*path*/) {
    last_error_ = "FFmpeg image backend not compiled in";
    return nullptr;
}

std::unique_ptr<Image>
ImageLoader::load_via_ffmpeg_memory(const uint8_t* /*data*/, size_t /*size*/,
                                     SourceFormat /*format*/) {
    last_error_ = "FFmpeg image backend not compiled in";
    return nullptr;
}

#endif // IDIFF_HAVE_FFMPEG_IMAGE_DECODE

} // namespace idiff
