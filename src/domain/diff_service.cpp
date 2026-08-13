#include "domain/diff_service.h"

#include "app/io/texture_uploader.h"
#include "core/channel_view.h"
#include "core/depth_utils.h"
#include "domain/selection_model.h"
#include "util/logger.h"
// Required so unique_ptr<Image> stored on DiffSlot can be destroyed
// when slots_ is cleared in this TU.
#include "core/image.h"        // IWYU pragma: keep
#include "core/image_impl.h"   // Image::Impl (for building single-channel Images)
#include "core/media_source.h" // IWYU pragma: keep

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <cstdint>
#include <cmath>
#include <limits>
#include <utility>

namespace idiff {

namespace {

// Build a temporary single-channel Image from the source Image by
// extracting the channel selected by `mode`.  Returns nullptr when the
// mode is None/RGB (meaning "use all channels") or when the extraction
// is not applicable to the source format.
std::unique_ptr<Image> extract_channel_image(const Image& src,
                                              ChannelViewMode mode) {
    if (mode == ChannelViewMode::None || mode == ChannelViewMode::RGB)
        return nullptr;

    // extract_channel_view returns a single-channel (or composited)
    // cv::Mat.  We use ViewBackground::Black as it is irrelevant for
    // single-channel extractions.
    auto mat_opt = extract_channel_view(src.mat(), mode,
                                        ViewBackground::Black);
    if (!mat_opt) return nullptr;

    cv::Mat chan = std::move(*mat_opt);
    auto img = std::make_unique<Image>();
    img->internal().mat = std::move(chan);
    img->internal().info = src.info();
    // Update format metadata to reflect the extracted result.
    img->internal().info.has_alpha = false;
    return img;
}

cv::Mat comparison_mat(const cv::Mat& source, ChannelViewMode mode) {
    cv::Mat selected = source;
    if (mode != ChannelViewMode::None && mode != ChannelViewMode::RGB) {
        auto extracted = extract_channel_view(source, mode, ViewBackground::Black);
        if (extracted) selected = std::move(*extracted);
    }
    cv::Mat result = convert_to_8u(selected);
    if (result.channels() == 1) {
        cv::cvtColor(result, result, cv::COLOR_GRAY2RGB);
    } else if (result.channels() == 4) {
        cv::cvtColor(result, result, cv::COLOR_RGBA2RGB);
    }
    return result;
}

cv::Mat heatmap_region(const cv::Mat& a, const cv::Mat& b,
                       const DiffService::Options& opts,
                       double normalization_max) {
    cv::Mat aa = comparison_mat(a, opts.channel_mode);
    cv::Mat bb = comparison_mat(b, opts.channel_mode);
    cv::Mat diff;
    cv::absdiff(aa, bb, diff);
    if (opts.amplification != 1.0) {
        diff.convertTo(diff, -1, opts.amplification);
    }
    cv::Mat gray;
    cv::cvtColor(diff, gray, cv::COLOR_RGB2GRAY);
    if (opts.heatmap_color == HeatmapColor::Gray) return gray;

    cv::Mat normalized;
    const double scale = normalization_max > 0.0
        ? 255.0 / normalization_max : 0.0;
    gray.convertTo(normalized, CV_8U, scale);
    int color_map = cv::COLORMAP_INFERNO;
    if (opts.heatmap_color == HeatmapColor::Viridis) {
        color_map = cv::COLORMAP_VIRIDIS;
    } else if (opts.heatmap_color == HeatmapColor::Coolwarm) {
        color_map = cv::COLORMAP_COOL;
    }
    cv::Mat colored;
    cv::applyColorMap(normalized, colored, color_map);
    cv::cvtColor(colored, colored, cv::COLOR_BGR2RGB);
    return colored;
}

cv::Mat scaled_region(const cv::Mat& source,
                      int target_width, int target_height,
                      const cv::Rect& target_roi) {
    if (source.cols == target_width && source.rows == target_height) {
        return source(target_roi);
    }
    const double sx = static_cast<double>(source.cols) / target_width;
    const double sy = static_cast<double>(source.rows) / target_height;
    const int x0 = std::clamp(
        static_cast<int>(std::floor(target_roi.x * sx)), 0, source.cols - 1);
    const int y0 = std::clamp(
        static_cast<int>(std::floor(target_roi.y * sy)), 0, source.rows - 1);
    const int x1 = std::clamp(
        static_cast<int>(std::ceil((target_roi.x + target_roi.width) * sx)),
        x0 + 1, source.cols);
    const int y1 = std::clamp(
        static_cast<int>(std::ceil((target_roi.y + target_roi.height) * sy)),
        y0 + 1, source.rows);
    cv::Mat result;
    cv::resize(source(cv::Rect(x0, y0, x1 - x0, y1 - y0)), result,
               target_roi.size(), 0.0, 0.0, cv::INTER_LANCZOS4);
    return result;
}

double difference_max_striped(const cv::Mat& a, const cv::Mat& b,
                              int target_width, int target_height,
                              const DiffService::Options& opts) {
    double maximum = 0.0;
    constexpr int kStripRows = 256;
    for (int y = 0; y < target_height; y += kStripRows) {
        const int rows = std::min(kStripRows, target_height - y);
        const cv::Rect roi(0, y, target_width, rows);
        cv::Mat heat = heatmap_region(
            scaled_region(a, target_width, target_height, roi),
            scaled_region(b, target_width, target_height, roi),
            {opts.amplification, HeatmapColor::Gray, opts.channel_mode},
            255.0);
        double strip_max = 0.0;
        cv::minMaxLoc(heat, nullptr, &strip_max);
        maximum = std::max(maximum, strip_max);
    }
    return maximum;
}

} // namespace

DiffService::DiffService(ITextureUploader& uploader)
    : uploader_(uploader) {}

DiffService::~DiffService() {
    clear();
}

void DiffService::clear() {
    for (auto& slot : slots_) {
        if (slot.texture) {
            uploader_.destroy(slot.texture);
            slot.texture = nullptr;
        }
        for (auto& tile : slot.texture_tiles) {
            uploader_.destroy(tile.texture);
        }
        slot.texture_tiles.clear();
    }
    slots_.clear();
}

void DiffService::update(const std::vector<ImageEntry>& entries,
                        const SelectionModel& selection,
                        const Options& opts,
                        std::string& out_error) {
    if (!dirty_) return;
    dirty_ = false;

    // Tear down any previously-uploaded textures before recomputing.
    // The full vector is discarded every refresh because the set of
    // partners (and their A counterpart) is tiny (typically <= 6) and
    // always reconstructed from the selection anyway.
    clear();

    if (selection.size() < 2) return;

    int idx_ref = -1;
    selection.get_ref_index(idx_ref);
    if (idx_ref < 0 || idx_ref >= static_cast<int>(entries.size())) return;

    const auto& entry_ref = entries[idx_ref];
    // Lazy-load: ensure the reference entry's pixels are resident
    // before reading them.  Under the lazy-load model a selected
    // entry may have been evicted by the LRU; ensure_decoded()
    // re-fetches from source.  safe on a const ImageEntry because
    // image / display_image / image_decoded are mutable.
    entry_ref.ensure_decoded();
    const auto* img_ref = entry_ref.display_image ? entry_ref.display_image.get()
                                                  : entry_ref.image.get();
    if (!img_ref) return;

    // When a single-channel view is active, extract that channel from
    // the reference once and reuse it for every partner comparison.
    std::unique_ptr<Image> chan_ref =
        extract_channel_image(*img_ref, opts.channel_mode);
    const Image* eff_ref = chan_ref ? chan_ref.get() : img_ref;

    // Build the partner list: every selected entry other than the
    // reference, in natural selection order.  This matches the viewport
    // slot order so the metrics table rows and viewport cells align.
    std::vector<int> partners;
    partners.reserve(selection.size());
    for (int s : selection.indices()) {
        if (s == idx_ref) continue;
        partners.push_back(s);
    }

    ImageComparator comparator;
    DifferenceOptions diff_opts;
    diff_opts.amplification = opts.amplification;
    diff_opts.heatmap_color = opts.heatmap_color;

    for (int partner : partners) {
        if (partner < 0 || partner >= static_cast<int>(entries.size())) continue;
        const auto& entry_p = entries[partner];
        // Lazy-load: fetch partner pixels on demand so a partner
        // that was evicted by the LRU still appears in the diff
        // column instead of being silently skipped.
        entry_p.ensure_decoded();
        const auto* img_p = entry_p.display_image ? entry_p.display_image.get()
                                                  : entry_p.image.get();
        if (!img_p) continue;

        // Extract matching channel from the partner when a channel
        // view is active.
        std::unique_ptr<Image> chan_p = extract_channel_image(*img_p, opts.channel_mode);
        const Image* eff_p = chan_p ? chan_p.get() : img_p;

        const auto limits = uploader_.limits();
        const int proxy_limit = std::min(
            4096, std::max(1, std::min(
                limits.max_width > 0 ? limits.max_width : 4096,
                limits.max_height > 0 ? limits.max_height : 4096)));
        const int target_width =
            std::max(eff_ref->mat().cols, eff_p->mat().cols);
        const int target_height =
            std::max(eff_ref->mat().rows, eff_p->mat().rows);
        if (target_width > proxy_limit || target_height > proxy_limit) {
            const double scale = std::min(
                static_cast<double>(proxy_limit) / target_width,
                static_cast<double>(proxy_limit) / target_height);
            const cv::Size proxy_size(
                std::max(1, static_cast<int>(target_width * scale)),
                std::max(1, static_cast<int>(target_height * scale)));
            cv::Mat proxy_a;
            cv::Mat proxy_b;
            cv::resize(eff_ref->mat(), proxy_a, proxy_size, 0.0, 0.0,
                       cv::INTER_AREA);
            cv::resize(eff_p->mat(), proxy_b, proxy_size, 0.0, 0.0,
                       cv::INTER_AREA);

            DiffSlot slot;
            slot.partner_entry_idx = partner;
            slot.tex_w = target_width;
            slot.tex_h = target_height;
            slot.normalization_max =
                difference_max_striped(eff_ref->mat(), eff_p->mat(),
                                       target_width, target_height, opts);
            slot.image = std::make_unique<Image>();
            slot.image->internal().mat =
                heatmap_region(proxy_a, proxy_b, opts, slot.normalization_max);
            slot.image->internal().info = eff_ref->info();
            slot.image->internal().info.width = slot.image->mat().cols;
            slot.image->internal().info.height = slot.image->mat().rows;
            slots_.push_back(std::move(slot));
            upload_slot(slots_.back());
            continue;
        }

        auto diff = comparator.compute_difference(*eff_ref, *eff_p, diff_opts);
        if (!diff) {
            const auto err = "Diff: " + comparator.last_error();
            LOG_WARN("%s", err.c_str());
            if (!out_error.empty()) out_error += " | ";
            out_error += err;
            continue;
        }
        auto heatmap = comparator.compute_heatmap(*diff, diff_opts);
        if (!heatmap) {
            const auto err = "Heatmap: " + comparator.last_error();
            LOG_WARN("%s", err.c_str());
            if (!out_error.empty()) out_error += " | ";
            out_error += err;
            continue;
        }

        DiffSlot slot;
        slot.partner_entry_idx = partner;
        slot.image = std::move(heatmap);
        slots_.push_back(std::move(slot));
        upload_slot(slots_.back());
    }
}

void DiffService::update_visible_tiles(
        const std::vector<ImageEntry>& entries,
        const SelectionModel& selection,
        const Options& opts,
        const std::vector<VisibleImageRegion>& regions,
        std::size_t gpu_budget_bytes) {
    if (slots_.empty() || regions.empty()) return;
    int ref_index = -1;
    selection.get_ref_index(ref_index);
    if (ref_index < 0 || ref_index >= static_cast<int>(entries.size())) return;
    const auto& ref_entry = entries[static_cast<std::size_t>(ref_index)];
    if (!ref_entry.ensure_decoded()) return;
    const Image* ref = ref_entry.display_image
        ? ref_entry.display_image.get() : ref_entry.image.get();
    if (!ref) return;

    const auto limits = uploader_.limits();
    const int tile_w = std::max(
        1, std::min(
            limits.max_width > 0 ? limits.max_width : kTextureTileDimension,
            kTextureTileDimension));
    const int tile_h = std::max(
        1, std::min(
            limits.max_height > 0 ? limits.max_height : kTextureTileDimension,
            kTextureTileDimension));
    ++tile_tick_;
    int uploaded = 0;

    for (const auto& region : regions) {
        if (!region.difference || region.slot < 0 ||
            region.slot >= static_cast<int>(slots_.size())) continue;
        auto& slot = slots_[static_cast<std::size_t>(region.slot)];
        if (!slot.image ||
            (slot.image->mat().cols == slot.tex_w &&
             slot.image->mat().rows == slot.tex_h)) continue;
        if (slot.partner_entry_idx < 0 ||
            slot.partner_entry_idx >= static_cast<int>(entries.size())) continue;
        const auto& partner_entry =
            entries[static_cast<std::size_t>(slot.partner_entry_idx)];
        if (!partner_entry.ensure_decoded()) continue;
        const Image* partner = partner_entry.display_image
            ? partner_entry.display_image.get() : partner_entry.image.get();
        if (!partner) continue;

        const int first_x = std::max(0, region.x / tile_w - 1);
        const int first_y = std::max(0, region.y / tile_h - 1);
        const int last_x = std::min(
            (slot.tex_w - 1) / tile_w,
            (region.x + region.width - 1) / tile_w + 1);
        const int last_y = std::min(
            (slot.tex_h - 1) / tile_h,
            (region.y + region.height - 1) / tile_h + 1);
        for (int ty = first_y; ty <= last_y; ++ty) {
            for (int tx = first_x; tx <= last_x; ++tx) {
                const int x = tx * tile_w;
                const int y = ty * tile_h;
                auto existing = std::find_if(
                    slot.texture_tiles.begin(), slot.texture_tiles.end(),
                    [&](const TextureTile& tile) {
                        return tile.x == x && tile.y == y;
                    });
                if (existing != slot.texture_tiles.end()) {
                    existing->last_used_frame = tile_tick_;
                    continue;
                }
                if (uploaded >= 4) continue;
                const int width = std::min(tile_w, slot.tex_w - x);
                const int height = std::min(tile_h, slot.tex_h - y);
                const cv::Rect roi(x, y, width, height);
                cv::Mat heat = heatmap_region(
                    scaled_region(ref->mat(), slot.tex_w, slot.tex_h, roi),
                    scaled_region(partner->mat(), slot.tex_w, slot.tex_h, roi),
                    opts, slot.normalization_max);
                cv::Mat rgba = convert_to_rgba8(heat);
                if (rgba.empty()) continue;
                UploadRequest request;
                request.pixels = rgba.ptr<std::uint8_t>();
                request.width = rgba.cols;
                request.height = rgba.rows;
                request.channels = 4;
                request.row_stride = rgba.step[0];
                request.linear_filter = false;
                SDL_Texture* texture = uploader_.upload(request);
                if (!texture) continue;
                slot.texture_tiles.push_back(
                    {texture, x, y, width, height, tile_tick_});
                ++uploaded;
            }
        }
    }

    auto bytes = [](const TextureTile& tile) {
        return static_cast<std::size_t>(tile.width) *
               static_cast<std::size_t>(tile.height) * 4u;
    };
    std::size_t total = 0;
    for (const auto& slot : slots_) {
        for (const auto& tile : slot.texture_tiles) total += bytes(tile);
    }
    while (total > gpu_budget_bytes) {
        DiffSlot* owner = nullptr;
        std::size_t oldest_index = 0;
        std::uint64_t oldest_tick = std::numeric_limits<std::uint64_t>::max();
        for (auto& slot : slots_) {
            for (std::size_t i = 0; i < slot.texture_tiles.size(); ++i) {
                if (slot.texture_tiles[i].last_used_frame < oldest_tick) {
                    oldest_tick = slot.texture_tiles[i].last_used_frame;
                    owner = &slot;
                    oldest_index = i;
                }
            }
        }
        if (!owner) break;
        total -= bytes(owner->texture_tiles[oldest_index]);
        uploader_.destroy(owner->texture_tiles[oldest_index].texture);
        owner->texture_tiles.erase(
            owner->texture_tiles.begin() +
            static_cast<std::ptrdiff_t>(oldest_index));
    }
}

void DiffService::upload_slot(DiffSlot& slot) {
    if (!slot.image) return;

    const auto& mat = slot.image->mat();
    if (mat.empty()) return;

    const int w = mat.cols;
    const int h = mat.rows;

    // Convert to 8-bit RGBA for SDL texture upload. Handles any depth
    // (CV_8U, CV_16U, CV_32F) and any channel count (1, 3, 4).
    cv::Mat upload_mat = convert_to_rgba8(mat);
    if (upload_mat.empty()) return;

    if (slot.texture) {
        uploader_.destroy(slot.texture);
        slot.texture = nullptr;
    }

    if (!upload_mat.isContinuous()) {
        upload_mat = upload_mat.clone();
    }
    UploadRequest req;
    req.pixels = upload_mat.ptr<std::uint8_t>();
    req.width = w;
    req.height = h;
    req.channels = 4;
    req.row_stride = upload_mat.step[0];
    slot.texture = uploader_.upload(req);
    if (!slot.texture) return;

    if (slot.tex_w <= 0) slot.tex_w = w;
    if (slot.tex_h <= 0) slot.tex_h = h;
}

} // namespace idiff
