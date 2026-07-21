#ifndef IDIFF_DOMAIN_IMAGE_LIBRARY_H
#define IDIFF_DOMAIN_IMAGE_LIBRARY_H

// Image library service.
//
// Owns the in-memory collection of ImageEntry records and the SDL
// textures attached to them.  This service is intentionally
// "container-only": it does not know about selection state, A/B
// assignment, the diff cache, or the timeline.  Operations that
// reorder or remove entries return the index remapping so the caller
// can update those derived structures atomically.
//
// Threading: not thread-safe.  All operations must run on the main
// (UI) thread, the same one that owns the SDL renderer.
//
// Ownership: the library borrows an ITextureUploader* with lifetime
// at least as long as the library itself.  It does not own the
// uploader.
//
// ImageEntry is currently a plain aggregate defined in app/app.h;
// the library re-uses it instead of introducing a parallel data
// structure during this incremental refactor.

#include "app/app.h"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace idiff {

class ITextureUploader;

class ImageLibrary {
public:
    // Marker the remap functions use for "this slot was deleted".
    static constexpr int kRemoved = -1;

    explicit ImageLibrary(ITextureUploader& uploader) noexcept;
    ~ImageLibrary();

    ImageLibrary(const ImageLibrary&) = delete;
    ImageLibrary& operator=(const ImageLibrary&) = delete;

    // Append a fully-constructed entry.  Texture, display_image and
    // measurements are taken as-is; the caller owns sequencing of
    // texture upload (call upload(size()-1) afterwards if desired).
    void add(ImageEntry entry);

    // Destroy the texture (if any) of an existing entry and erase it
    // from the vector.  Indices >= `index` shift down by one; the
    // returned remap table maps "old index -> new index", with
    // kRemoved at the deleted slot.  Out-of-range indices are
    // ignored and an empty remap is returned.
    std::vector<int> remove(std::size_t index);

    // Move entry at `from` to position `to`.  Returns a remap table
    // describing the permutation so callers can update parallel
    // index-keyed state (e.g. selection sets).  No-op (and an
    // empty remap) when the indices are equal or out of range.
    std::vector<int> move(std::size_t from, std::size_t to);

    // Sort entries by filename (case-insensitive).  Returns a remap
    // table "old index -> new index" so callers can rewrite their
    // selection sets.
    std::vector<int> sort_by_filename();

    // Sort entries with a caller-supplied "less" predicate over the
    // entry's filename.  Used by App::sort_entries_by_name() which
    // applies a stem/extension-aware ordering that case-insensitive
    // sort cannot express.  Returns the same kind of remap table as
    // sort_by_filename().
    using FilenameLess = std::function<bool(const std::string& a,
                                            const std::string& b)>;
    std::vector<int> sort_with(const FilenameLess& less);

    // Destroy all textures and drop every entry.
    void clear();

    // Release decoded pixel data and GPU texture for a single entry
    // without removing it from the library.  Destroys the texture via
    // the uploader first, then resets pixel state via
    // ImageEntry::release_pixel_data.  cached_info and source are
    // preserved so the entry can be re-decoded on demand.  Used by
    // the lazy-load eviction path.
    void release_entry_pixels(std::size_t index);

    // Texture lifecycle helpers.  The pixel-conversion logic
    // (channel-view extraction, RGBA conversion) currently lives in
    // App; once the viewport is decoupled the responsibility will
    // move here.  For now, callers prepare the RGBA buffer and ask
    // the library to upload it.
    void upload(std::size_t index, const class UploadRequest& req);

    // Replace the texture of an entry with a freshly-uploaded one,
    // destroying the previous one if any.  Useful for diff slots that
    // are stored outside the library.
    static void replace_texture(class ITextureUploader& uploader,
                                struct DiffSlot& slot,
                                const class UploadRequest& req);

    // Read-only views.
    std::size_t size() const noexcept { return entries_.size(); }
    bool empty() const noexcept { return entries_.empty(); }
    const ImageEntry& at(std::size_t index) const { return entries_.at(index); }
    ImageEntry& at(std::size_t index) { return entries_.at(index); }
    const std::vector<ImageEntry>& all() const noexcept { return entries_; }
    std::vector<ImageEntry>& all() noexcept { return entries_; }

private:
    ITextureUploader& uploader_;
    std::vector<ImageEntry> entries_;
};

} // namespace idiff

#endif // IDIFF_DOMAIN_IMAGE_LIBRARY_H
