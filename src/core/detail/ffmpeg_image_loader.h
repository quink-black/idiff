// SPDX-License-Identifier: BSD-2-Clause
#ifndef IDIFF_FFMPEG_IMAGE_LOADER_H
#define IDIFF_FFMPEG_IMAGE_LOADER_H

#ifdef IDIFF_HAVE_FFMPEG_IMAGE_DECODE

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "core/image.h"

namespace idiff {

// Decode a HEIF / AVIF still image from a memory buffer using
// libavformat + libavcodec (+ libavfilter for tile-grid composition).
//
// Behaviour:
//   * Opens the buffer as an in-memory AVFormatContext (no file path
//     traffic, so non-ASCII paths on Windows are not a problem).
//   * Prefers an AV_STREAM_GROUP_PARAMS_TILE_GRID stream group as
//     the main image entry point; if the file has none (single-tile
//     primary item) falls back to av_find_best_stream(VIDEO).
//   * Decodes every tile stream to one AVFrame each.
//   * Multi-tile path: composes the tiles via HeifTileAssembler
//     (libavfilter xstack + crop) into a single AVFrame.
//   * Single-tile path: skips the filter graph entirely.
//   * Converts the final AVFrame to RGB24 / RGBA / RGB48LE / RGBA64LE
//     and fills an Image (cv::Mat + ImageInfo) compatible with the
//     rest of the idiff pipeline.
//
// `flags` is a bitwise-OR of LoadFlag values from image_loader.h --
// LoadFlag::Keep16Bit and LoadFlag::KeepAlpha decide the target
// pixel format.  ICC profile presence is recorded via
// `info.icc_profile_name = "Embedded ICC"` when found, but never
// applied (matching the ImageMagick path's "tag, don't transform"
// convention).
//
// On failure, returns nullptr and fills `err` with a human-readable
// message that already includes the failing tile / stream index.
//
// Thread safety: re-entrant; each call constructs its own contexts.
std::unique_ptr<Image> load_heif_avif_from_memory(const uint8_t* data,
                                                  std::size_t size,
                                                  SourceFormat fmt,
                                                  std::uint32_t flags,
                                                  std::string& err);

} // namespace idiff

#endif // IDIFF_HAVE_FFMPEG_IMAGE_DECODE
#endif // IDIFF_FFMPEG_IMAGE_LOADER_H
