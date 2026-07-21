#include "app/controller.h"

#include "app/sr_dialog.h"
#include "app/sr_infer_engine.h"
#include "app/sr_infer_engine_factory.h"
#include "app/status_reporter.h"
#include "core/image.h"
#include "core/image_loader.h"
#include "core/media_source.h"
#ifdef IDIFF_HAVE_FFMPEG
#include "core/video_file_source.h"
#endif
#include "domain/comparison_config_service.h"
#include "domain/diff_service.h"
#include "domain/group_key.h"
#include "domain/image_library.h"
#include "domain/selection_model.h"
#include "domain/sr_task_service.h"
#include "domain/timeline_model.h"
#include "util/logger.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace idiff {

bool ImageEntry::ensure_decoded() {
    if (image_decoded && image) return true;
    if (!source) return false;
    auto img = source->read_frame(cached_frame);
    if (!img) return false;
    cached_info = img->info();
    image = std::move(img);
    image_decoded = true;
    return true;
}

AppController::AppController(ITextureUploader& texture_uploader,
                             IStatusReporter& status_reporter)
    : library_(std::make_unique<ImageLibrary>(texture_uploader)),
      selection_(std::make_unique<SelectionModel>()),
      timeline_(std::make_unique<TimelineModel>()),
      diff_(std::make_unique<DiffService>(texture_uploader)),
      sr_tasks_(std::make_unique<SrTaskService>()),
      comparison_config_(std::make_unique<ComparisonConfigService>()),
      status_reporter_(&status_reporter) {}

AppController::~AppController() = default;

ImageLibrary& AppController::library() noexcept { return *library_; }
SelectionModel& AppController::selection() noexcept { return *selection_; }
TimelineModel& AppController::timeline() noexcept { return *timeline_; }
DiffService& AppController::diff() noexcept { return *diff_; }
SrTaskService& AppController::sr_tasks() noexcept { return *sr_tasks_; }
ComparisonConfigService& AppController::comparison_config() noexcept {
    return *comparison_config_;
}

namespace {

// Compare filenames by (stem, extension) so that a "base" name like
// "kid.jpg" sorts before its derived siblings ("kid-pisa.jpg",
// "kid-pisa-v0.jpg").  The default std::string operator< puts '-'
// (0x2D) before '.' (0x2E) which is the wrong answer here: the user
// expects the shorter root name to come first.
bool filename_less(const std::string& a, const std::string& b) {
    auto [a_stem, a_ext] = idiff::split_stem_ext(a);
    auto [b_stem, b_ext] = idiff::split_stem_ext(b);
    if (a_stem != b_stem) return a_stem < b_stem;
    return a_ext < b_ext;
}

} // namespace

void AppController::get_ref_index(int& ref_idx) const {
    selection_->get_ref_index(ref_idx);
}

int AppController::timeline_length() const {
    return TimelineModel::length(library_->all());
}

void AppController::compute_display_labels() {
    auto& entries = library_->all();
    if (entries.empty()) return;

    std::unordered_map<std::string, int> name_counts;
    for (const auto& entry : entries) {
        name_counts[entry.filename]++;
    }

    for (auto& entry : entries) {
        if (name_counts[entry.filename] > 1) {
            auto sep = entry.path.find_last_of("/\\");
            if (sep != std::string::npos) {
                auto parent = entry.path.substr(0, sep);
                auto sep2 = parent.find_last_of("/\\");
                std::string dir_name = (sep2 != std::string::npos)
                    ? parent.substr(sep2 + 1) : parent;
                entry.display_label = dir_name + "/" + entry.filename;
            } else {
                entry.display_label = entry.filename;
            }
        } else {
            entry.display_label = entry.filename;
        }
    }
}

void AppController::sort_entries_by_name() {
    auto remap = library_->sort_with(&filename_less);
    selection_->apply_remap(remap);
}

std::set<int> AppController::group_indices(int index) const {
    const int n = static_cast<int>(library_->all().size());
    if (index < 0 || index >= n) return {};

    const auto& entries = library_->all();
    std::string key = group_key_from_filename(entries[index].filename);

    std::set<int> result;
    for (int i = 0; i < n; ++i) {
        if (group_key_from_filename(entries[i].filename) == key) {
            result.insert(i);
        }
    }
    return result;
}

bool AppController::select_group(int index) {
    auto group = group_indices(index);
    if (group.empty()) return false;
    bool changed = selection_->replace(std::move(group));
    if (changed) diff_->mark_dirty();
    apply_comparison_reference();
    return changed;
}

AppController::GroupClickAction AppController::click_in_group(int index) {
    auto group = group_indices(index);
    if (group.empty()) return GroupClickAction::Noop;

    // Decide whether the click stays inside the currently selected
    // group or crosses over.  "Inside" means every selected index
    // already belongs to the clicked entry's group (and there is at
    // least one selected entry to anchor the comparison; an empty
    // selection is treated as "no active group" and therefore counts
    // as crossing over so the user gets a useful initial selection).
    const auto& sel = selection_->indices();
    bool inside = !sel.empty();
    for (int s : sel) {
        if (group.find(s) == group.end()) {
            inside = false;
            break;
        }
    }

    if (inside) {
        // Same group: toggle just this single entry so the user can
        // narrow / widen the comparison within the active group.
        selection_->toggle(index);
        diff_->mark_dirty();
        return GroupClickAction::Toggled;
    }

    // Crossing groups: replace the selection with every member of
    // the new group, matching the "click another group switches and
    // selects all" UX.
    bool changed = selection_->replace(std::move(group));
    if (changed) diff_->mark_dirty();
    apply_comparison_reference();
    return GroupClickAction::Switched;
}

bool AppController::select_range(int from, int to) {
    const int n = static_cast<int>(library_->all().size());
    if (n == 0) return false;
    from = std::max(0, std::min(from, n - 1));
    to = std::max(0, std::min(to, n - 1));
    if (from > to) std::swap(from, to);

    std::set<int> range;
    for (int i = from; i <= to; ++i) range.insert(i);
    bool changed = selection_->replace(std::move(range));
    if (changed) diff_->mark_dirty();
    return changed;
}

void AppController::move_entry(int from, int to) {
    if (from == to) return;
    const int n = static_cast<int>(library_->all().size());
    if (from < 0 || from >= n) return;
    if (to < 0 || to >= n) return;

    auto remap = library_->move(static_cast<std::size_t>(from),
                                static_cast<std::size_t>(to));
    selection_->apply_remap(remap);
    // Reordering can change which entry plays the reference role
    // (selection is a sorted set so the smallest index is the
    // reference) and invalidates the partner_entry_idx values cached
    // inside DiffService's slots.  Mark the diff dirty so it
    // recomputes against the new mapping; without this the viewport
    // keeps showing the previous heatmap pixels even though the slot
    // labels have shifted to point at different entries.
    diff_->mark_dirty();
}

void AppController::mark_as_reference(int index) {
    const int n = static_cast<int>(library_->all().size());
    if (index < 0 || index >= n) return;

    std::string key = comparison_key_of(index);

    // Preserve the "selection lives in one comparison" invariant
    // that the GUI's Group-by-Name UI relies on.  When the current
    // selection is a coherent single-comparison set AND the new
    // entry belongs to a different comparison, narrow the selection
    // to the new comparison before inserting -- the same behaviour
    // as click_in_group()'s group-switch branch.
    //
    // Heterogeneous / empty selections (flat-mode usage) fall
    // through to the legacy insert path so cross-comparison flat
    // selections still work.
    //
    // This matters most for RPC sweeps that mark a reference per
    // comparison: without the narrowing, each call would grow the
    // selection across comparison boundaries and leave the viewport
    // diffing unrelated images.  With the narrowing, only the
    // last-marked entry's comparison stays resident in selection;
    // every previous mark is recorded in comparison_reference_ for
    // use when its comparison is revisited.
    const auto& sel = selection_->indices();
    if (!sel.empty() && !key.empty()) {
        std::string active = comparison_key_of(*sel.begin());
        if (active != key && !active.empty()) {
            bool coherent = true;
            for (int s : sel) {
                if (comparison_key_of(s) != active) {
                    coherent = false;
                    break;
                }
            }
            if (coherent) {
                auto members = group_indices(index);
                if (!members.empty()) {
                    selection_->replace(std::move(members));
                }
            }
        }
    }

    // Ensure the entry is part of the selection so overlay / diff
    // actually use it, then designate it as the reference.  No
    // library reordering needed -- the explicit reference decouples
    // "which image is A" from "which image has the smallest index".
    selection_->insert(index);
    selection_->set_reference(index);

    // Persist the choice per comparison so switching away and back
    // keeps the same reference.  The key for an entry depends on
    // whether a comparison config is active or not (see
    // comparison_key_of).
    if (!key.empty()) {
        comparison_reference_[key] = library_->all()[index].path;
    }

    diff_->mark_dirty();
}

namespace {

// Build the comparison key for an entry given the current comparison
// config state.  Two namespaces ("config:" / "file:") so a session
// that mixes a config load with a later flat load never collides.
std::string compute_comparison_key(const idiff::ImageEntry& entry,
                                   const idiff::ComparisonConfigService& cfg) {
    if (cfg.has_config()) {
        int idx = cfg.current_index();
        if (idx < 0) return {};
        const auto& groups = cfg.config().groups;
        if (idx >= static_cast<int>(groups.size())) return {};
        const auto& name = groups[idx].name;
        if (!name.empty()) return "config:" + name;
        return "config:#" + std::to_string(idx);
    }
    return "file:" + idiff::group_key_from_filename(entry.filename);
}

} // namespace

std::string AppController::comparison_key_of(int index) const {
    const int n = static_cast<int>(library_->all().size());
    if (index < 0 || index >= n) return {};
    return compute_comparison_key(library_->all()[index], *comparison_config_);
}

std::vector<AppController::ComparisonView>
AppController::list_comparisons() const {
    std::vector<ComparisonView> result;
    const auto& entries = library_->all();

    if (comparison_config_->has_config()) {
        const auto& groups = comparison_config_->config().groups;
        const int current = comparison_config_->current_index();
        result.reserve(groups.size());
        for (std::size_t i = 0; i < groups.size(); ++i) {
            ComparisonView cv;
            const auto& name = groups[i].name;
            if (!name.empty()) {
                cv.key = "config:" + name;
                cv.name = name;
            } else {
                cv.key = "config:#" + std::to_string(i);
                cv.name = "Group " + std::to_string(i + 1);
            }
            cv.current = (static_cast<int>(i) == current);
            // Only the resident comparison has loaded entries.  The
            // rest stay empty -- callers can still
            // set_comparison_reference() for them by key, the mapping
            // is applied on next switch.
            if (cv.current) {
                for (int j = 0; j < static_cast<int>(entries.size()); ++j) {
                    cv.entries.push_back(j);
                }
            }
            result.push_back(std::move(cv));
        }
        return result;
    }

    // Filename-stem comparisons.  Build them in insertion order so
    // the output is stable across calls.
    std::unordered_map<std::string, std::size_t> key_to_idx;
    for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
        std::string stem = group_key_from_filename(entries[i].filename);
        std::string key = "file:" + stem;
        auto it = key_to_idx.find(key);
        if (it == key_to_idx.end()) {
            key_to_idx[key] = result.size();
            ComparisonView cv;
            cv.key = std::move(key);
            cv.name = std::move(stem);
            cv.current = true;
            cv.entries.push_back(i);
            result.push_back(std::move(cv));
        } else {
            result[it->second].entries.push_back(i);
        }
    }
    return result;
}

bool AppController::set_comparison_reference(const std::string& key,
                                             const std::string& path) {
    if (key.empty()) return false;
    if (path.empty()) {
        comparison_reference_.erase(key);
    } else {
        comparison_reference_[key] = path;
    }
    // If the modified key matches the currently active comparison,
    // apply immediately so the GUI reflects the new reference without
    // waiting for a comparison switch.
    apply_comparison_reference();
    return true;
}

void AppController::apply_comparison_reference() {
    const auto& sel = selection_->indices();
    if (sel.empty()) return;

    const auto& entries = library_->all();
    // Use any selected entry to determine the active comparison key.
    int probe = *sel.begin();
    if (probe < 0 || probe >= static_cast<int>(entries.size())) return;
    std::string key = compute_comparison_key(entries[probe],
                                             *comparison_config_);
    if (key.empty()) return;

    auto it = comparison_reference_.find(key);
    if (it == comparison_reference_.end()) return;
    const std::string& path = it->second;

    for (int idx : sel) {
        if (idx < 0 || idx >= static_cast<int>(entries.size())) continue;
        if (entries[idx].path == path) {
            selection_->set_reference(idx);
            diff_->mark_dirty();
            return;
        }
    }
    // Path not present in current selection.  Leave the implicit
    // reference (smallest index) in place; the recorded mapping is
    // kept for the next comparison switch.
}

void AppController::remove_entry(int index) {
    const int n = static_cast<int>(library_->all().size());
    if (index < 0 || index >= n) return;

    // The library destroys the entry's texture via ITextureUploader
    // and returns a remap so we can fix up selection indices.
    auto remap = library_->remove(static_cast<std::size_t>(index));
    selection_->apply_remap(remap);

    compute_display_labels();
    diff_->mark_dirty();
}

bool AppController::has_running_sr_tasks() const {
    return sr_tasks_->has_running();
}

void AppController::sync_entries_to_timeline() {
    std::string status_buf;
    if (timeline_->sync_to(library_->all(), status_buf)) {
        diff_->mark_dirty();
    }
    // sync_to() only writes to out_status on a per-frame read failure.
    // Use append so the message decorates whatever prior text the
    // status bar was showing (matching the previous in-place behaviour
    // when state_->status_text was passed directly).
    if (!status_buf.empty()) {
        status_reporter_->append_status(status_buf);
    }
}

void AppController::preview_entries_to_timeline() {
    timeline_->preview_to(library_->all());
}

void AppController::start_sr_task(const SRTaskParams& params) {
    auto factory = []() -> std::unique_ptr<SRInferEngine> {
        return SRInferEngineFactory::instance().create_engine("seedvr2");
    };
    const auto result = sr_tasks_->start(params, factory);
    if (!result.ok) {
        status_reporter_->show_error(result.error_title,
                                     result.error_message);
    }
}

LoaderBackend AppController::loader_backend() const noexcept {
    return loader_backend_;
}

void AppController::set_loader_backend(LoaderBackend backend) noexcept {
    loader_backend_ = backend;
}

namespace {

// Build the right MediaSource for `path`, with a fallback so files
// whose extension is not in our hard-coded video list (e.g. `.y4m`,
// raw `.yuv`, `.ivf`) still get a chance through the FFmpeg-backed
// VideoFileSource when the regular still-image loaders give up.
//
// On success returns an opened source plus a freshly decoded frame 0.
// On failure returns {nullptr, nullptr} and writes a combined error
// describing what every attempted backend reported, so the status
// bar / log line mentions both the image and the video failure.
struct OpenedSource {
    std::unique_ptr<idiff::MediaSource> source;
    std::unique_ptr<idiff::Image> first_frame;
};

OpenedSource open_media_source(const std::string& path,
                               idiff::LoaderBackend backend,
                               std::string& out_error) {
    OpenedSource result;

#ifdef IDIFF_HAVE_FFMPEG
    // Files with a known video container extension always go straight
    // to the video pipeline -- no point in burning two failed image
    // decode passes first.
    if (idiff::is_video_file_extension(path)) {
        auto vsrc = std::make_unique<idiff::VideoFileSource>(path);
        if (!vsrc->is_valid()) {
            out_error = vsrc->last_error();
            return result;
        }
        auto img = vsrc->read_frame(0);
        if (!img) {
            out_error = vsrc->last_error();
            return result;
        }
        result.source = std::move(vsrc);
        result.first_frame = std::move(img);
        return result;
    }
#endif

    // Try the still-image loaders first.  ImageLoader already
    // fails over between OpenCV / ImageMagick / FFmpeg-image-decode
    // internally, so a failure here means none of the configured
    // image backends recognised the file.
    auto isrc = std::make_unique<idiff::ImageFileSource>(path, backend);
    auto img = isrc->read_frame(0);
    if (img) {
        result.source = std::move(isrc);
        result.first_frame = std::move(img);
        return result;
    }
    std::string image_err = isrc->last_error();

#ifdef IDIFF_HAVE_FFMPEG
    // Last-chance fallback: hand the file to FFmpeg as if it were a
    // video container.  This is what makes `.y4m` and other formats
    // FFmpeg knows about but our image stack does not load through
    // the image dialog without forcing the user to rename the file.
    auto vsrc = std::make_unique<idiff::VideoFileSource>(path);
    if (vsrc->is_valid()) {
        auto vimg = vsrc->read_frame(0);
        if (vimg) {
            result.source = std::move(vsrc);
            result.first_frame = std::move(vimg);
            return result;
        }
        out_error = "image: " + image_err
                  + "; FFmpeg: " + vsrc->last_error();
        return result;
    }
    out_error = "image: " + image_err
              + "; FFmpeg: " + vsrc->last_error();
    return result;
#else
    out_error = std::move(image_err);
    return result;
#endif
}

} // namespace

namespace {

// Recreate the MediaSource for an entry from its on-disk path so the
// decoder re-opens the file (picking up new content after an external
// mv or overwrite).  Returns the freshly decoded frame 0 on success,
// or nullptr on failure.  On success the entry's source is replaced;
// on failure the previous source is left intact.
std::unique_ptr<idiff::Image>
reopen_source(idiff::ImageEntry& entry, idiff::LoaderBackend backend) {
    std::string err;
    auto opened = open_media_source(entry.path, backend, err);
    if (!opened.first_frame) return nullptr;
    entry.source = std::move(opened.source);
    return std::move(opened.first_frame);
}

} // namespace

void AppController::reload_all_images() {
    auto& entries = library_->all();
    if (entries.empty()) return;

    int reloaded = 0;
    int failed = 0;
    std::string last_fail;
    std::string failure_details;
    for (auto& entry : entries) {
        auto img = reopen_source(entry, loader_backend_);
        if (img) {
            entry.cached_info = img->info();
            entry.image = std::move(img);
            entry.image_decoded = true;
            entry.release_pixels();
            entry.texture_dirty = true;
            ++reloaded;
        } else {
            ++failed;
            std::string err = entry.source ? entry.source->last_error()
                                           : "no source";
            last_fail = entry.filename + " (" + err + ")";
            if (!failure_details.empty()) failure_details += "\n";
            failure_details += entry.filename + "\n  " + err;
        }
    }
    diff_->mark_dirty();

    const char* name = ImageLoader::backend_name(loader_backend_);
    std::string status;
    if (failed == 0) {
        status = "Reloaded " + std::to_string(reloaded)
               + " image(s) via " + name;
    } else {
        status = "Reloaded " + std::to_string(reloaded) + " via " + name
               + ", " + std::to_string(failed) + " failed: " + last_fail;
    }
    status_reporter_->set_status(status);

    // Status-bar text is easy to miss, especially when several
    // entries failed and only the last one fits.  Raise a modal so
    // the user actually notices and sees every failure at once.
    if (failed > 0) {
        status_reporter_->show_error("Reload failed", failure_details);
    }
}

void AppController::reload_entry(int index) {
    const int n = static_cast<int>(library_->all().size());
    if (index < 0 || index >= n) return;

    auto& entry = library_->all()[index];
    auto img = reopen_source(entry, loader_backend_);
    if (img) {
        entry.cached_info = img->info();
        entry.image = std::move(img);
        entry.image_decoded = true;
        entry.release_pixels();
        entry.texture_dirty = true;
        diff_->mark_dirty();
    } else {
        std::string err = entry.source ? entry.source->last_error()
                                       : "no source";
        status_reporter_->set_status("Reload failed: " + entry.filename
                                     + " (" + err + ")");
        status_reporter_->show_error("Reload failed",
                                     entry.filename + "\n  " + err);
    }
}

int AppController::reload_entries_by_path(
        const std::vector<std::string>& paths) {
    auto& entries = library_->all();
    int reloaded = 0;
    for (const auto& path : paths) {
        for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
            if (entries[i].path != path) continue;
            auto img = reopen_source(entries[i], loader_backend_);
            if (img) {
                entries[i].cached_info = img->info();
                entries[i].image = std::move(img);
                entries[i].image_decoded = true;
                entries[i].release_pixels();
                entries[i].texture_dirty = true;
                ++reloaded;
            }
            break;
        }
    }
    if (reloaded > 0) {
        diff_->mark_dirty();
        status_reporter_->set_status(
            "Reloaded " + std::to_string(reloaded) + " changed file(s)");
    }
    return reloaded;
}

AppController::LoadImagesResult
AppController::load_images(const std::vector<std::string>& paths) {
    LoadImagesResult result;

    // Detect "first load" before any append so a later auto-select
    // does not clobber the user's existing picks on subsequent
    // append-style calls.
    const bool was_empty = library_->all().empty();

    // Collect every failed path so we can raise a single modal at
    // the end.  The status bar already gets a per-file note via
    // set_status() below, but that text is trivial to miss when the
    // user picked a batch and walked away.
    struct LoadFailure {
        std::string path;
        std::string error;
    };
    std::vector<LoadFailure> failures;

    for (const auto& path : paths) {
        // Deduplicate: if this path is already loaded, refresh the
        // existing entry instead of appending a duplicate.
        bool found_existing = false;
        for (auto& existing : library_->all()) {
            if (existing.path == path) {
                if (auto* ifs = dynamic_cast<ImageFileSource*>(
                        existing.source.get())) {
                    ifs->set_preferred_backend(loader_backend_);
                }
                auto img = existing.source
                    ? existing.source->read_frame(0) : nullptr;
                if (img) {
                    existing.cached_info = img->info();
                    existing.image = std::move(img);
                    existing.image_decoded = true;
                    existing.release_pixels();
                    existing.texture_dirty = true;
                    status_reporter_->set_status("Refreshed: " + path);
                }
                found_existing = true;
                break;
            }
        }
        if (found_existing) continue;

        // Open the file through whichever backend recognises it.
        // open_media_source picks VideoFileSource for known video
        // extensions, otherwise tries the still-image loaders first
        // and finally falls back to FFmpeg so formats outside our
        // hard-coded extension list (e.g. .y4m) still load.
        std::string open_err;
        auto opened = open_media_source(path, loader_backend_, open_err);
        if (!opened.first_frame) {
            status_reporter_->set_status("Failed to load: " + path
                                         + " (" + open_err + ")");
            LOG_WARN("load_images failed: %s (%s)",
                     path.c_str(), open_err.c_str());
            failures.push_back({path, open_err});
            continue;
        }
        auto source = std::move(opened.source);
        auto img = std::move(opened.first_frame);

        {
            ImageEntry entry;
            entry.path = path;
            auto sep = path.find_last_of("/\\");
            entry.filename = (sep != std::string::npos) ? path.substr(sep + 1)
                                                        : path;
            entry.display_label = entry.filename;
            // Show frame count for multi-frame sources (video files)
            if (source->frame_count() > 1) {
                entry.display_label += " (" + std::to_string(source->frame_count())
                                    + " frames)";
            }
            entry.source = std::move(source);
            // Cache metadata from the first frame so it is available
            // even after pixels are released, then immediately free
            // the decoded pixel data.  The image will be re-decoded
            // on demand when this entry enters the selection.
            entry.cached_info = img->info();
            entry.image = std::move(img);
            entry.image_decoded = true;
            entry.release_pixels();
            entry.display_image = nullptr;
            entry.texture = nullptr;
            entry.texture_dirty = true;

            library_->add(std::move(entry));
            status_reporter_->set_status("Loaded: " + path);
        }
    }

    sort_entries_by_name();
    compute_display_labels();
    diff_->mark_dirty();

    if (!failures.empty()) {
        std::string body;
        for (const auto& f : failures) {
            if (!body.empty()) body += "\n\n";
            body += f.path + "\n  " + f.error;
        }
        const std::string title = failures.size() == 1
            ? std::string("Failed to load image")
            : ("Failed to load " + std::to_string(failures.size())
               + " images");
        status_reporter_->show_error(title, body);
    }

    // First-load convenience: pick the first up-to-two entries as A
    // and B and flag them for texture refresh.  The viewport mode
    // change (Overlay) is left to the caller because it lives in
    // the UI layer.
    if (was_empty && !library_->all().empty()) {
        selection_->clear();
        const int n = static_cast<int>(library_->all().size());
        const int pick = std::min(2, n);
        for (int i = 0; i < pick; ++i) selection_->insert(i);
        for (int s : selection_->indices()) {
            if (s >= 0 && s < n) {
                library_->all()[s].texture_dirty = true;
            }
        }
        diff_->mark_dirty();
        result.did_first_load_select = true;
    }

    // Whenever the selection changed (first-load auto-select OR an
    // append-style call that didn't change selection at all), make
    // sure the active comparison's recorded reference is honoured.
    // Cheap when no rule is recorded for the active comparison.
    apply_comparison_reference();

    return result;
}

AppController::SwitchGroupResult
AppController::load_comparison_config(const std::string& path) {
    SwitchGroupResult result;
    auto load_result = comparison_config_->load(path);
    if (!load_result.ok) {
        status_reporter_->set_status(load_result.status_message);
        return result;
    }

    // Drop whatever was previously loaded so the user sees a clean
    // switch.  Only one group's worth of pixels is kept resident; the
    // group-switch path enforces the same invariant on subsequent
    // navigation.
    library_->clear();
    selection_->clear();
    diff_->clear();
    diff_->mark_dirty();
    // A fresh config is a new world.  Per-comparison references
    // recorded for the previous session would just be confusing dead
    // entries.
    comparison_reference_.clear();

    status_reporter_->set_status(load_result.status_message);

    if (comparison_config_->has_config()) {
        result = switch_to_comparison_group(0);
    }
    return result;
}

AppController::SwitchGroupResult
AppController::switch_to_comparison_group(int group_idx) {
    SwitchGroupResult result;
    if (group_idx == comparison_config_->current_index()) return result;

    // Release the previous group's images first so we never hold two
    // groups' pixels in memory simultaneously.  This is the main
    // memory lever for configs with many large groups.
    library_->clear();
    selection_->clear();
    diff_->clear();
    diff_->mark_dirty();

    auto switch_result = comparison_config_->switch_to(group_idx);
    if (!switch_result.ok) {
        status_reporter_->set_status(switch_result.status_message);
        return result;
    }

    if (!switch_result.entries.empty()) {
        std::vector<std::string> local_paths;
        local_paths.reserve(switch_result.entries.size());
        for (const auto& e : switch_result.entries) {
            local_paths.push_back(e.local_path);
        }
        auto load_result = load_images(local_paths);
        result.did_first_load_select = load_result.did_first_load_select;
    }

    // Apply the human-friendly labels supplied by the service so the
    // image list shows config titles instead of opaque cache
    // filenames.  Match by path because load_images() may have
    // re-ordered through sort_entries_by_name().
    auto& entries = library_->all();
    if (!entries.empty()) {
        std::unordered_map<std::string, std::string> label_by_path;
        for (const auto& e : switch_result.entries) {
            if (!e.display_label.empty()) {
                label_by_path[e.local_path] = e.display_label;
            }
        }
        if (!label_by_path.empty()) {
            for (auto& e : entries) {
                auto it = label_by_path.find(e.path);
                if (it == label_by_path.end()) continue;
                e.filename = it->second;
                e.display_label = it->second;
            }
            // compute_display_labels() will uniquify duplicates.
            compute_display_labels();
        }
    }

    status_reporter_->set_status(switch_result.status_message);
    // After load_images() has populated library + selection, apply
    // the per-comparison reference recorded for this config group
    // (if any).
    apply_comparison_reference();
    return result;
}

} // namespace idiff
