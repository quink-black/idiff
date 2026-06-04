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

    // Move the entry to the top so it becomes the smallest selected
    // index (and therefore the reference) regardless of whether the
    // user had already selected it.  apply_remap follows the move.
    if (index != 0) {
        auto remap = library_->move(static_cast<std::size_t>(index),
                                    static_cast<std::size_t>(0));
        selection_->apply_remap(remap);
    }
    // Ensure the new top entry is part of the selection so overlay /
    // diff actually use it.  No-op when the entry was already
    // selected.
    selection_->insert(0);

    diff_->mark_dirty();
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

// Recreate the MediaSource for an entry from its on-disk path so the
// decoder re-opens the file (picking up new content after an external
// mv or overwrite).  Returns the freshly decoded frame 0 on success,
// or nullptr on failure.  On success the entry's source is replaced;
// on failure the previous source is left intact.
std::unique_ptr<idiff::Image>
reopen_source(idiff::ImageEntry& entry, idiff::LoaderBackend backend) {
    std::unique_ptr<idiff::MediaSource> new_source;
#ifdef IDIFF_HAVE_FFMPEG
    if (idiff::is_video_file_extension(entry.path)) {
        auto vsrc = std::make_unique<idiff::VideoFileSource>(entry.path);
        if (!vsrc->is_valid()) return nullptr;
        new_source = std::move(vsrc);
    } else
#endif
    {
        new_source = std::make_unique<idiff::ImageFileSource>(
            entry.path, backend);
    }

    auto img = new_source->read_frame(0);
    if (!img) return nullptr;

    entry.source = std::move(new_source);
    return img;
}

} // namespace

void AppController::reload_all_images() {
    auto& entries = library_->all();
    if (entries.empty()) return;

    int reloaded = 0;
    int failed = 0;
    std::string last_fail;
    for (auto& entry : entries) {
        auto img = reopen_source(entry, loader_backend_);
        if (img) {
            entry.image = std::move(img);
            entry.display_image.reset();
            entry.texture_dirty = true;
            ++reloaded;
        } else {
            ++failed;
            std::string err = entry.source ? entry.source->last_error()
                                           : "no source";
            last_fail = entry.filename + " (" + err + ")";
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
}

void AppController::reload_entry(int index) {
    const int n = static_cast<int>(library_->all().size());
    if (index < 0 || index >= n) return;

    auto& entry = library_->all()[index];
    auto img = reopen_source(entry, loader_backend_);
    if (img) {
        entry.image = std::move(img);
        entry.display_image.reset();
        entry.texture_dirty = true;
        diff_->mark_dirty();
    } else {
        std::string err = entry.source ? entry.source->last_error()
                                       : "no source";
        status_reporter_->set_status("Reload failed: " + entry.filename
                                     + " (" + err + ")");
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
                entries[i].image = std::move(img);
                entries[i].display_image.reset();
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
                    existing.image = std::move(img);
                    existing.display_image.reset();
                    existing.texture_dirty = true;
                    status_reporter_->set_status("Refreshed: " + path);
                }
                found_existing = true;
                break;
            }
        }
        if (found_existing) continue;

        // Create the appropriate MediaSource based on file type.
        // Video container files (MP4, MKV, etc.) get a VideoFileSource
        // that supports multi-frame decoding; everything else gets an
        // ImageFileSource for single-frame still images.
        std::unique_ptr<MediaSource> source;
#ifdef IDIFF_HAVE_FFMPEG
        if (is_video_file_extension(path)) {
            auto vsrc = std::make_unique<VideoFileSource>(path);
            if (!vsrc->is_valid()) {
                const auto err = vsrc->last_error();
                status_reporter_->set_status("Failed to load: " + path
                                             + " (" + err + ")");
                LOG_WARN("load_images failed: %s (%s)",
                         path.c_str(), err.c_str());
                continue;
            }
            source = std::move(vsrc);
        } else
#endif
        {
            source = std::make_unique<ImageFileSource>(path, loader_backend_);
        }

        auto img = source->read_frame(0);
        if (img) {
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
            entry.image = std::move(img);
            entry.display_image = nullptr;
            entry.texture = nullptr;
            entry.texture_dirty = true;

            library_->add(std::move(entry));
            status_reporter_->set_status("Loaded: " + path);
        } else {
            const auto err = source->last_error();
            status_reporter_->set_status("Failed to load: " + path
                                         + " (" + err + ")");
            LOG_WARN("load_images failed: %s (%s)",
                     path.c_str(), err.c_str());
        }
    }

    sort_entries_by_name();
    compute_display_labels();
    diff_->mark_dirty();

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
    return result;
}

} // namespace idiff
