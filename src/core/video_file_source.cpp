#ifdef IDIFF_HAVE_FFMPEG

#include "core/video_file_source.h"
#include "core/image.h"
#include "core/image_impl.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace idiff {

// ============================================================================
// is_video_file_extension
// ============================================================================

bool is_video_file_extension(const std::string& path) noexcept {
    // Extract extension and lowercase it
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return false;

    std::string ext = path.substr(dot);
    for (auto& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    // Common video container extensions
    static const char* const video_exts[] = {
        ".mp4", ".mkv", ".mov", ".avi", ".webm", ".flv",
        ".ts", ".m4v", ".wmv", ".mpg", ".mpeg", ".3gp",
        ".mts", ".m2ts", ".vob", ".ogv", ".f4v",
    };

    for (const auto* ve : video_exts) {
        if (ext == ve) return true;
    }
    return false;
}

// ============================================================================
// VideoFileSource
// ============================================================================

VideoFileSource::VideoFileSource(std::string path)
    : path_(std::move(path))
    , decoder_(std::make_unique<VideoDecoder>())
{
    if (!decoder_->open(path_)) {
        last_error_ = decoder_->last_error();
        return;
    }

    // Build format description: "h264 1920x1080 8-bit" or "h264 1920x1080 8-bit (rotated 90°)"
    format_desc_ = decoder_->codec_name() + " "
                 + std::to_string(decoder_->width()) + "x"
                 + std::to_string(decoder_->height()) + " "
                 + std::to_string(decoder_->bit_depth()) + "-bit";
    if (decoder_->detected_rotation() != VideoRotation::None) {
        format_desc_ += " (rotated "
                     + std::to_string(static_cast<int>(decoder_->detected_rotation()))
                     + "\xC2\xB0)";  // UTF-8 degree sign
    }
}

VideoFileSource::~VideoFileSource() = default;

bool VideoFileSource::is_valid() const noexcept {
    return decoder_ && decoder_->is_open();
}

int VideoFileSource::frame_count() const noexcept {
    return decoder_ ? decoder_->frame_count() : 0;
}

int VideoFileSource::width() const noexcept {
    return decoder_ ? decoder_->width() : 0;
}

int VideoFileSource::height() const noexcept {
    return decoder_ ? decoder_->height() : 0;
}

const std::string& VideoFileSource::format_description() const noexcept {
    return format_desc_;
}

const std::string& VideoFileSource::last_error() const noexcept {
    return last_error_;
}

std::unique_ptr<Image> VideoFileSource::read_frame(int index) {
    if (!is_valid()) {
        last_error_ = "video source not open";
        return nullptr;
    }

    if (index < 0 || index >= decoder_->frame_count()) {
        last_error_ = "frame index out of range";
        return nullptr;
    }

    cv::Mat rgb = decoder_->decode_frame(index);
    if (rgb.empty()) {
        last_error_ = decoder_->last_error();
        return nullptr;
    }

    // Grab a refcounted handle to the source-domain AVFrame for this
    // same index.  decode_frame() above already populated the
    // decoder's cached snapshot, so decode_frame_raw(index) here is a
    // pure refcount bump on the existing buffers -- no second decode,
    // no pixel copy.  The Image takes ownership and releases the ref
    // in its destructor.  A nullptr (decode_frame_raw failure) is not
    // fatal: the Image still carries the rgb mat, the pixel sampler
    // will simply fall back to 8-bit values for this frame.
    AVFrame* src_frame = decoder_->decode_frame_raw(index);

    auto img = std::make_unique<Image>();
    img->internal().mat = rgb;
    img->internal().src_av_frame = src_frame;
    img->internal().info.width = rgb.cols;
    img->internal().info.height = rgb.rows;
    img->internal().info.pixel_format = PixelFormat::RGB8;
    img->internal().info.source_format = SourceFormat::Unknown;
    img->internal().info.bit_depth = 8;
    img->internal().info.source_bit_depth = decoder_->bit_depth();
    img->internal().info.has_alpha = false;
    img->internal().info.color_space = "BT.709";

    last_error_.clear();
    return img;
}

std::unique_ptr<Image> VideoFileSource::read_keyframe(int index) {
    if (!is_valid()) {
        last_error_ = "video source not open";
        return nullptr;
    }

    if (index < 0 || index >= decoder_->frame_count()) {
        last_error_ = "frame index out of range";
        return nullptr;
    }

    cv::Mat rgb = decoder_->decode_keyframe(index);
    if (rgb.empty()) {
        last_error_ = decoder_->last_error();
        return nullptr;
    }

    auto img = std::make_unique<Image>();
    img->internal().mat = rgb;
    img->internal().info.width = rgb.cols;
    img->internal().info.height = rgb.rows;
    img->internal().info.pixel_format = PixelFormat::RGB8;
    img->internal().info.source_format = SourceFormat::Unknown;
    img->internal().info.bit_depth = 8;
    img->internal().info.source_bit_depth = decoder_->bit_depth();
    img->internal().info.has_alpha = false;
    img->internal().info.color_space = "BT.709";

    last_error_.clear();
    return img;
}

} // namespace idiff

#endif // IDIFF_HAVE_FFMPEG
