#ifndef IDIFF_DOMAIN_DIFF_SERVICE_H
#define IDIFF_DOMAIN_DIFF_SERVICE_H

// Diff service.
//
// Owns the per-frame difference cache: one DiffSlot per partner image
// compared against the reference (the smallest selected entry index).
// Each slot keeps the heatmap Image and an SDL texture handle
// uploaded through the injected ITextureUploader.
//
// The service is invalidated lazily.  Anything that can change the
// inputs (selection membership, frame index, decoder reload, options
// tweak, ...) must call mark_dirty(); the next update() call discards
// the previous slots and recomputes from scratch.
//
// The service deliberately does not know about App, ImGui, or any of
// the App::State fields.  Callers pass the entries vector and the
// SelectionModel by const reference plus an Options struct, and the
// service writes any error messages into a caller-owned string sink.

#include "app/app.h"
#include "core/channel_view.h"
#include "core/image_comparator.h"

#include <cstddef>
#include <string>
#include <vector>

namespace idiff {

class ITextureUploader;
class SelectionModel;

class DiffService {
public:
    // Inputs the heatmap pipeline reads on every recompute.  Decoupled
    // from App::State so the service can be driven from tests with
    // synthetic numbers.
    struct Options {
        // Multiplier applied to per-pixel differences before colormap
        // lookup; matches DifferenceOptions::amplification.  > 0.
        double amplification = 5.0;
        HeatmapColor heatmap_color = HeatmapColor::Inferno;
        // When set to a single-channel mode (R, G, B, Alpha, Y, U, V),
        // the diff is computed on that channel only.  ChannelViewMode::None
        // and ChannelViewMode::RGB mean "diff all RGB channels" (the
        // previous default behaviour).
        ChannelViewMode channel_mode = ChannelViewMode::None;
    };

    explicit DiffService(ITextureUploader& uploader);
    ~DiffService();

    DiffService(const DiffService&) = delete;
    DiffService& operator=(const DiffService&) = delete;

    // Mark the cache stale.  Cheap; the actual work happens in
    // update() so we don't recompute multiple times per frame when
    // several callers invalidate before a single render pass.
    void mark_dirty() noexcept { dirty_ = true; }
    bool is_dirty() const noexcept { return dirty_; }

    // Destroy every slot's texture (via the injected uploader) and
    // empty the vector.  Safe to call repeatedly.  Does not change
    // the dirty flag (callers may want it set so the next update()
    // recomputes a fresh cache).
    void clear();

    // Recompute the slots if the cache is dirty.  No-op when clean.
    //
    // Builds one slot per "partner" entry: every selected entry other
    // than the reference (smallest selected index), in natural
    // selection order, so the slot order matches the viewport's cell
    // order.  A single per-partner failure (compute_difference or
    // compute_heatmap returning null) appends a " | "-separated
    // message into out_error and continues with the next partner so
    // a single bad input cannot blank out the entire diff column.
    //
    // After update() returns, is_dirty() is false even when no slots
    // were produced (e.g. fewer than two selected entries); the
    // caller must call mark_dirty() again to retry.
    void update(const std::vector<ImageEntry>& entries,
                const SelectionModel& selection,
                const Options& opts,
                std::string& out_error);

    // Read-only views consumed by the UI layer.  Pointers and sizes
    // are stable until the next clear() / update() call.
    const std::vector<DiffSlot>& slots() const noexcept { return slots_; }
    bool empty() const noexcept { return slots_.empty(); }
    std::size_t size() const noexcept { return slots_.size(); }

private:
    // Upload a freshly-computed heatmap into slot.texture.  Replaces
    // any pre-existing texture on the slot.  Silently leaves
    // slot.texture as null on uploader failure; the slot is still
    // valid for displaying placeholder text.
    void upload_slot(DiffSlot& slot);

    ITextureUploader& uploader_;
    std::vector<DiffSlot> slots_;
    bool dirty_ = true;
};

} // namespace idiff

#endif // IDIFF_DOMAIN_DIFF_SERVICE_H
