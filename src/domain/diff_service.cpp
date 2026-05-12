#include "domain/diff_service.h"

#include "app/io/texture_uploader.h"
#include "core/channel_view.h"
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

    int idx_a = -1, idx_b = -1;
    selection.get_ab_indices(idx_a, idx_b);
    if (idx_a < 0 || idx_a >= static_cast<int>(entries.size())) return;

    const auto& entry_a = entries[idx_a];
    const auto* img_a = entry_a.display_image ? entry_a.display_image.get()
                                              : entry_a.image.get();
    if (!img_a) return;

    // When a single-channel view is active, extract that channel from A
    // once and reuse it for every partner comparison.
    std::unique_ptr<Image> chan_a = extract_channel_image(*img_a, opts.channel_mode);
    const Image* eff_a = chan_a ? chan_a.get() : img_a;

    // Build the partner order to match the viewport's slot order: B
    // first (the second entry from get_ab_indices), then any other
    // selected entries in their natural selection order.  This keeps
    // the visual layout predictable and makes the metrics table row
    // order match what the viewport shows.
    std::vector<int> partners;
    partners.reserve(selection.size());
    if (idx_b >= 0 && idx_b < static_cast<int>(entries.size())) {
        partners.push_back(idx_b);
    }
    for (int s : selection.indices()) {
        if (s == idx_a) continue;
        if (s == idx_b) continue;
        partners.push_back(s);
    }

    ImageComparator comparator;
    DifferenceOptions diff_opts;
    diff_opts.amplification = opts.amplification;
    diff_opts.heatmap_color = opts.heatmap_color;

    for (int partner : partners) {
        if (partner < 0 || partner >= static_cast<int>(entries.size())) continue;
        const auto& entry_p = entries[partner];
        const auto* img_p = entry_p.display_image ? entry_p.display_image.get()
                                                  : entry_p.image.get();
        if (!img_p) continue;

        // Extract matching channel from the partner when a channel
        // view is active.
        std::unique_ptr<Image> chan_p = extract_channel_image(*img_p, opts.channel_mode);
        const Image* eff_p = chan_p ? chan_p.get() : img_p;

        auto diff = comparator.compute_difference(*eff_a, *eff_p, diff_opts);
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

void DiffService::upload_slot(DiffSlot& slot) {
    if (!slot.image) return;

    const auto& mat = slot.image->mat();
    if (mat.empty()) return;

    const int w = mat.cols;
    const int h = mat.rows;
    const int channels = mat.channels();

    // The heatmap arrives in RGB order from image_comparator; SDL
    // texture upload expects RGBA32, so widen to four channels.
    cv::Mat upload_mat;
    if (channels == 4) {
        upload_mat = mat;
    } else if (channels == 3) {
        cv::cvtColor(mat, upload_mat, cv::COLOR_RGB2RGBA);
    } else if (channels == 1) {
        cv::cvtColor(mat, upload_mat, cv::COLOR_GRAY2RGBA);
    } else {
        return;
    }

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
    slot.texture = uploader_.upload(req);
    if (!slot.texture) return;

    slot.tex_w = w;
    slot.tex_h = h;
}

} // namespace idiff
