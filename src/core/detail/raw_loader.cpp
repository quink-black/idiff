#include "core/detail/raw_loader.h"
#include "core/image_impl.h"
#include "core/detail/platform_utf8.h"

#include <algorithm>

// LibRaw headers auto-enable OpenMP on macOS when _REENTRANT is defined,
// but clang on macOS doesn't ship omp.h. Undefine before including.
#ifdef __APPLE__
#undef _OPENMP
#undef _REENTRANT
#endif

#include <libraw/libraw.h>
#include <opencv2/core.hpp>

namespace idiff {

namespace {

const char* raw_extensions[] = {
    ".dng", ".cr2", ".nef", ".arw", ".rw2",
    ".orf", ".pef", ".sr2", ".srw", ".raw",
    ".raf", ".mrw", ".x3f", ".3fr", ".iiq",
    ".kdc", ".dcr", ".erf", ".mef", ".mos",
    ".nrw", ".ptx", ".r3d", ".rwz", ".srf",
};

} // namespace

std::unique_ptr<Image> RawLoader::load(const std::string& path) {
    last_error_.clear();

    auto raw = std::unique_ptr<LibRaw>(new LibRaw);

    int ret;
#ifdef _WIN32
    // LibRaw::open_file(const char*) 内部使用 fopen()，在 Windows 上
    // 无法处理 UTF-8 编码的非 ASCII 路径。改用宽字符重载版本。
    auto wide = platform::utf8_to_wide(path);
    ret = raw->open_file(wide.c_str());
#else
    ret = raw->open_file(path.c_str());
#endif
    if (ret != LIBRAW_SUCCESS) {
        last_error_ = "LibRaw failed to open: " + std::string(libraw_strerror(ret));
        return nullptr;
    }

    ret = raw->unpack();
    if (ret != LIBRAW_SUCCESS) {
        last_error_ = "LibRaw failed to unpack: " + std::string(libraw_strerror(ret));
        return nullptr;
    }

    ret = raw->dcraw_process();
    if (ret != LIBRAW_SUCCESS) {
        last_error_ = "LibRaw failed to process: " + std::string(libraw_strerror(ret));
        return nullptr;
    }

    int errcode = 0;
    libraw_processed_image_t* processed = raw->dcraw_make_mem_image(&errcode);
    if (!processed) {
        last_error_ = "LibRaw failed to produce output image (error code: " +
                      std::to_string(errcode) + ")";
        return nullptr;
    }

    cv::Mat mat;
    if (processed->colors == 3) {
        mat = cv::Mat(processed->height, processed->width, CV_8UC3, processed->data);
    } else if (processed->colors == 1) {
        mat = cv::Mat(processed->height, processed->width, CV_8UC1, processed->data);
    }

    auto image = std::make_unique<Image>();
    image->internal().mat = mat.clone();
    image->internal().info.width = processed->width;
    image->internal().info.height = processed->height;
    image->internal().info.source_format = SourceFormat::RAW;
    image->internal().info.pixel_format = processed->colors == 1
        ? PixelFormat::Gray8 : PixelFormat::RGB8;
    image->internal().info.bit_depth = 8;
    image->internal().info.has_alpha = false;
    image->internal().info.color_space = "RAW";

    LibRaw::dcraw_clear_mem(processed);

    return image;
}

bool RawLoader::is_raw_extension(const std::string& path) {
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return false;

    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    for (const auto* raw_ext : raw_extensions) {
        if (ext == raw_ext) return true;
    }
    return false;
}

const std::string& RawLoader::last_error() const noexcept {
    return last_error_;
}

} // namespace idiff
