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

    int idx_ref = -1;
    selection.get_ref_index(idx_ref);
    if (idx_ref < 0 || idx_ref >= static_cast<int>(entries.size())) return;

    const auto& entry_ref = entries[idx_ref];
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
        const auto* img_p = entry_p.display_image ? entry_p.display_image.get()
                                                  : entry_p.image.get();
        if (!img_p) continue;

        // Extract matching channel from the partner when a channel
        // view is active.
        std::unique_ptr<Image> chan_p = extract_channel_image(*img_p, opts.channel_mode);
        const Image* eff_p = chan_p ? chan_p.get() : img_p;

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
    slot.texture = uploader_.upload(req);
    if (!slot.texture) return;

    slot.tex_w = w;
    slot.tex_h = h;
}

} // namespace idiff
