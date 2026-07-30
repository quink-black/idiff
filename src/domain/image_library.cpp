#include "domain/image_library.h"

#include "app/io/texture_uploader.h"
// IWYU pragma: keep -- ImageEntry holds unique_ptr<Image> /
// unique_ptr<MediaSource>; vector<ImageEntry>::erase / move-assign
// instantiate their destructors here, so the full types must be
// visible even though no explicit Image / MediaSource symbol is used.
#include "core/image.h"        // IWYU pragma: keep
#include "core/media_source.h" // IWYU pragma: keep
#include "domain/group_key.h"
#include "util/logger.h"

#include <algorithm>
#include <cctype>
#include <numeric>

namespace idiff {

namespace {

int icmp(const std::string& a, const std::string& b) noexcept {
    auto la = a.size();
    auto lb = b.size();
    auto n = std::min(la, lb);
    for (std::size_t i = 0; i < n; ++i) {
        unsigned char ca = static_cast<unsigned char>(std::tolower(a[i]));
        unsigned char cb = static_cast<unsigned char>(std::tolower(b[i]));
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    if (la == lb) return 0;
    return la < lb ? -1 : 1;
}

} // namespace

ImageLibrary::ImageLibrary(ITextureUploader& uploader) noexcept
    : uploader_(uploader) {}

ImageLibrary::~ImageLibrary() {
    clear();
}

void ImageLibrary::add(ImageEntry entry) {
    entries_.push_back(std::move(entry));
}

std::vector<int> ImageLibrary::remove(std::size_t index) {
    if (index >= entries_.size()) return {};

    uploader_.destroy(entries_[index].texture);
    entries_[index].texture = nullptr;

    const std::size_t old_size = entries_.size();
    entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(index));

    std::vector<int> remap(old_size, 0);
    for (std::size_t i = 0; i < old_size; ++i) {
        if (i < index) remap[i] = static_cast<int>(i);
        else if (i == index) remap[i] = kRemoved;
        else remap[i] = static_cast<int>(i - 1);
    }
    LOG_DEBUG("library remove(%zu) -> size=%zu", index, entries_.size());
    return remap;
}

std::vector<int> ImageLibrary::move(std::size_t from, std::size_t to) {
    const std::size_t n = entries_.size();
    if (from >= n || to >= n || from == to) return {};

    if (from < to) {
        ImageEntry tmp = std::move(entries_[from]);
        for (std::size_t i = from; i < to; ++i) {
            entries_[i] = std::move(entries_[i + 1]);
        }
        entries_[to] = std::move(tmp);
    } else {
        ImageEntry tmp = std::move(entries_[from]);
        for (std::size_t i = from; i > to; --i) {
            entries_[i] = std::move(entries_[i - 1]);
        }
        entries_[to] = std::move(tmp);
    }

    std::vector<int> remap(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        if (i == from) {
            remap[i] = static_cast<int>(to);
        } else if (from < to && i > from && i <= to) {
            remap[i] = static_cast<int>(i - 1);
        } else if (to < from && i >= to && i < from) {
            remap[i] = static_cast<int>(i + 1);
        } else {
            remap[i] = static_cast<int>(i);
        }
    }
    LOG_DEBUG("library move(%zu -> %zu)", from, to);
    return remap;
}

std::vector<int> ImageLibrary::sort_by_filename() {
    return sort_with([](const std::string& a, const std::string& b) {
        return icmp(a, b) < 0;
    });
}

std::vector<int> ImageLibrary::sort_with(const FilenameLess& less) {
    const std::size_t n = entries_.size();
    if (n < 2) {
        std::vector<int> identity(n);
        std::iota(identity.begin(), identity.end(), 0);
        return identity;
    }

    std::vector<std::size_t> order(n);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(),
              [&](std::size_t a, std::size_t b) {
                  return less(entries_[a].filename, entries_[b].filename);
              });

    std::vector<ImageEntry> sorted;
    sorted.reserve(n);
    std::vector<int> remap(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        std::size_t src = order[i];
        remap[src] = static_cast<int>(i);
        sorted.push_back(std::move(entries_[src]));
    }
    entries_ = std::move(sorted);
    LOG_DEBUG("library sort_with (n=%zu)", n);
    return remap;
}

std::vector<int> ImageLibrary::sort_by_directory() {
    const std::size_t n = entries_.size();
    if (n < 2) {
        std::vector<int> identity(n);
        std::iota(identity.begin(), identity.end(), 0);
        return identity;
    }

    std::vector<std::size_t> order(n);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(),
              [&](std::size_t a, std::size_t b) {
                  const std::string& pa = entries_[a].path;
                  const std::string& pb = entries_[b].path;
                  std::string da = group_key_from_directory(pa);
                  std::string db = group_key_from_directory(pb);
                  int c = icmp(da, db);
                  if (c != 0) return c < 0;
                  // Tie-break by filename (case-insensitive).
                  return icmp(entries_[a].filename,
                              entries_[b].filename) < 0;
              });

    std::vector<ImageEntry> sorted;
    sorted.reserve(n);
    std::vector<int> remap(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        std::size_t src = order[i];
        remap[src] = static_cast<int>(i);
        sorted.push_back(std::move(entries_[src]));
    }
    entries_ = std::move(sorted);
    LOG_DEBUG("library sort_by_directory (n=%zu)", n);
    return remap;
}

void ImageLibrary::clear() {
    for (auto& e : entries_) {
        uploader_.destroy(e.texture);
        e.texture = nullptr;
    }
    entries_.clear();
}

void ImageLibrary::release_entry_pixels(std::size_t index) {
    if (index >= entries_.size()) return;
    auto& e = entries_[index];
    uploader_.destroy(e.texture);
    e.release_pixel_data();
}

void ImageLibrary::upload(std::size_t index, const UploadRequest& req) {
    if (index >= entries_.size()) {
        LOG_WARN("library upload index out of range: %zu", index);
        return;
    }
    auto& e = entries_[index];
    uploader_.destroy(e.texture);
    e.texture = uploader_.upload(req);
    if (e.texture) {
        e.tex_w = req.width;
        e.tex_h = req.height;
        e.texture_dirty = false;
    } else {
        e.tex_w = 0;
        e.tex_h = 0;
    }
}

void ImageLibrary::replace_texture(ITextureUploader& uploader,
                                   DiffSlot& slot,
                                   const UploadRequest& req) {
    uploader.destroy(slot.texture);
    slot.texture = uploader.upload(req);
    if (slot.texture) {
        slot.tex_w = req.width;
        slot.tex_h = req.height;
    } else {
        slot.tex_w = 0;
        slot.tex_h = 0;
    }
}

} // namespace idiff
