#include "app/io/file_dialog.h"
#include "util/logger.h"

#include <nfd.h>

namespace idiff {

namespace {

// NFD's nfdfilteritem_t consumes raw nul-terminated strings; our
// FileDialogFilter holds string_views that may not be nul-terminated
// (they are, in practice -- string literals -- but we should not
// assume that on the public interface).  This adapter copies them
// into local std::string buffers and builds the C array.
struct NfdFilterBuf {
    std::vector<std::string> labels;
    std::vector<std::string> exts;
    std::vector<nfdfilteritem_t> items;

    explicit NfdFilterBuf(const std::vector<FileDialogFilter>& in) {
        labels.reserve(in.size());
        exts.reserve(in.size());
        items.reserve(in.size());
        for (const auto& f : in) {
            labels.emplace_back(f.label);
            exts.emplace_back(f.extensions);
            items.push_back(nfdfilteritem_t{labels.back().c_str(),
                                            exts.back().c_str()});
        }
    }
};

FileDialogResult make_error(const char* what) {
    FileDialogResult r;
    r.error = what ? what : "unknown file dialog error";
    LOG_WARN("file dialog error: %s", r.error.c_str());
    return r;
}

} // namespace

FileDialogResult NfdFileDialog::open_multiple(
    const std::vector<FileDialogFilter>& filters) {
    NfdFilterBuf buf(filters);
    const nfdpathset_t* out = nullptr;
    nfdresult_t res = NFD_OpenDialogMultiple(
        &out, buf.items.empty() ? nullptr : buf.items.data(),
        static_cast<nfdfiltersize_t>(buf.items.size()), nullptr);
    if (res == NFD_OKAY) {
        FileDialogResult r;
        nfdpathsetsize_t count = 0;
        NFD_PathSet_GetCount(out, &count);
        r.paths.reserve(count);
        for (nfdpathsetsize_t i = 0; i < count; ++i) {
            nfdchar_t* path = nullptr;
            NFD_PathSet_GetPath(out, i, &path);
            if (path) {
                r.paths.emplace_back(path);
                NFD_PathSet_FreePath(path);
            }
        }
        NFD_PathSet_Free(out);
        LOG_DEBUG("open_multiple selected %zu path(s)", r.paths.size());
        return r;
    }
    if (res == NFD_ERROR) return make_error(NFD_GetError());
    return {}; // cancelled
}

FileDialogResult NfdFileDialog::open_single(
    const std::vector<FileDialogFilter>& filters) {
    NfdFilterBuf buf(filters);
    nfdchar_t* out = nullptr;
    nfdresult_t res = NFD_OpenDialog(
        &out, buf.items.empty() ? nullptr : buf.items.data(),
        static_cast<nfdfiltersize_t>(buf.items.size()), nullptr);
    if (res == NFD_OKAY) {
        FileDialogResult r;
        if (out) {
            r.paths.emplace_back(out);
            NFD_FreePath(out);
        }
        LOG_DEBUG("open_single selected '%s'",
                  r.paths.empty() ? "" : r.paths.front().c_str());
        return r;
    }
    if (res == NFD_ERROR) return make_error(NFD_GetError());
    return {};
}

FileDialogResult NfdFileDialog::save(
    const std::vector<FileDialogFilter>& filters,
    std::string_view default_name) {
    NfdFilterBuf buf(filters);
    std::string default_name_buf(default_name);
    nfdchar_t* out = nullptr;
    nfdresult_t res = NFD_SaveDialog(
        &out, buf.items.empty() ? nullptr : buf.items.data(),
        static_cast<nfdfiltersize_t>(buf.items.size()), nullptr,
        default_name_buf.empty() ? nullptr : default_name_buf.c_str());
    if (res == NFD_OKAY) {
        FileDialogResult r;
        if (out) {
            r.paths.emplace_back(out);
            NFD_FreePath(out);
        }
        LOG_DEBUG("save dialog returned '%s'",
                  r.paths.empty() ? "" : r.paths.front().c_str());
        return r;
    }
    if (res == NFD_ERROR) return make_error(NFD_GetError());
    return {};
}

} // namespace idiff
