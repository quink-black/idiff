#ifndef IDIFF_APP_CONTROLLER_H
#define IDIFF_APP_CONTROLLER_H

#include "core/image_loader.h"

#include <memory>
#include <string>
#include <vector>

namespace idiff {

class ITextureUploader;
class IStatusReporter;
class ImageLibrary;
class SelectionModel;
class TimelineModel;
class DiffService;
class SrTaskService;
class ComparisonConfigService;
struct SRTaskParams;

// Owns every non-UI service that drives the application.  Constructed
// once per process inside App::init() after the platform IO seam (the
// texture uploader) is wired, and destroyed before that seam goes
// away.  The controller is the seam between the GUI shell (App) and
// the headless domain layer: tests instantiate the controller alone
// to exercise behaviour without bringing up SDL or ImGui.
//
// Business orchestration that touches only the domain services lives
// here.  Methods that still need access to UI-level state (status
// text, error dialog, viewport options, ...) remain on App pending a
// dedicated StatusReporter abstraction.
class AppController {
public:
    // Both collaborators are borrowed for the controller's lifetime.
    // Caller (App) must keep them alive until ~AppController returns.
    AppController(ITextureUploader& texture_uploader,
                  IStatusReporter& status_reporter);
    ~AppController();

    AppController(const AppController&) = delete;
    AppController& operator=(const AppController&) = delete;

    ImageLibrary& library() noexcept;
    SelectionModel& selection() noexcept;
    TimelineModel& timeline() noexcept;
    DiffService& diff() noexcept;
    SrTaskService& sr_tasks() noexcept;
    ComparisonConfigService& comparison_config() noexcept;

    // ---- Business orchestration ------------------------------------
    //
    // Each helper coordinates two or more services at once.  They are
    // exposed on the controller (rather than as inline call sequences
    // at every site) so the same flow can be exercised from headless
    // tests without bringing up the SDL / ImGui front-end.

    // Returns the entry indices used as A and B for overlay / diff.
    // Derived from the first two selected items (in selection order),
    // honouring the user-controlled swap flag.  Missing slots are -1.
    void get_ab_indices(int& a_idx, int& b_idx) const;

    // Length of the shared timeline, i.e. the maximum number of frames
    // across all multi-frame entries (clamped to at least 1).
    int timeline_length() const;

    // Recompute every entry's display_label so duplicates of the same
    // filename are disambiguated by their parent directory.  No-op on
    // an empty library.
    void compute_display_labels();

    // Sort the library by filename and patch the selection model so
    // selected indices follow their entries to the new positions.
    void sort_entries_by_name();

    // Move the entry at `from` to position `to` and patch the
    // selection accordingly.  Out-of-range indices are ignored; a
    // no-op move (from == to) returns immediately.
    void move_entry(int from, int to);

    // Remove the entry at `index` (destroying its texture via the
    // injected ITextureUploader), patch the selection, refresh
    // display labels and mark the diff dirty.  Resets the A/B swap
    // toggle if the removed entry was actually selected.  Out-of-
    // range indices are ignored.
    void remove_entry(int index);

    // True while at least one super-resolution task is still running.
    bool has_running_sr_tasks() const;

    // Re-decode every multi-frame entry so it matches the shared
    // timeline index (plus each entry's per-entry frame_offset).
    // No-op for libraries with only single-frame entries.  Marks the
    // diff cache dirty when any entry actually changes; failures are
    // reported through the injected status reporter.
    void sync_entries_to_timeline();

    // Fast approximate preview for scrubbing: uses read_keyframe()
    // instead of read_frame().  Updates viewport pixels but does not
    // mark the diff cache dirty (diff is too expensive for scrub).
    void preview_entries_to_timeline();

    // Spawn an SR engine for `params` and append it to the task queue.
    // The engine is built via the global SRInferEngineFactory; on
    // failure the error is forwarded to the status reporter as a modal
    // dialog and no task is enqueued.
    void start_sr_task(const SRTaskParams& params);

    // Image loader backend that load_images() / reload_all_images()
    // currently honour.  Owned here so the headless tests can switch
    // backends without going through the View menu, and so the
    // reload flow can stay in the controller.
    LoaderBackend loader_backend() const noexcept;
    void set_loader_backend(LoaderBackend backend) noexcept;

    // Re-decode every entry's current frame using loader_backend().
    // Marks textures dirty for entries that actually changed and
    // marks the diff cache dirty unconditionally so the next render
    // recomputes both views.  Reports the per-batch summary
    // ("Reloaded N image(s) via <backend>") through the status
    // reporter; entries whose decode fails keep their previous
    // pixel data.
    void reload_all_images();

    // Re-decode a single entry by index from its on-disk source.
    // Marks the entry's texture dirty and the diff cache dirty on
    // success; leaves the previous pixel data intact on failure.
    // Out-of-range indices are ignored.
    void reload_entry(int index);

    // Re-decode the entries whose paths appear in `paths`.  Paths
    // not currently in the library are silently skipped.  Returns
    // the number of entries actually reloaded.
    int reload_entries_by_path(const std::vector<std::string>& paths);

    // Outcome of load_images() the caller needs to act on.  The
    // controller does every domain-level side effect (add entries,
    // sort, label, mark diff dirty, run the first-load auto-select),
    // but the viewport's comparison mode is owned by the UI layer:
    // when did_first_load_select is true the caller should switch
    // the viewport to Overlay so the new selection is visible.
    struct LoadImagesResult {
        bool did_first_load_select = false;
    };

    // Load every path as a still image entry (callers must filter
    // out paths that need a YUV-parameter modal first, since this
    // method decodes synchronously and assumes the file is self-
    // describing).  Each entry is appended via the ImageLibrary,
    // the library is then re-sorted and re-labelled; per-file
    // status is reported through the status reporter.  Returns
    // true in did_first_load_select when the library was empty
    // before the call and at least one entry was added (the caller
    // should then put the viewport in Overlay mode).
    LoadImagesResult load_images(const std::vector<std::string>& paths);

    // Outcome propagated to the caller for comparison-config
    // navigation.  Mirrors LoadImagesResult: the controller does
    // every domain-level effect (load config, swap groups, drive
    // load_images, relabel) and tells the caller when the UI
    // viewport mode should change.
    struct SwitchGroupResult {
        bool did_first_load_select = false;
    };

    // Parse the JSON comparison config at `path` via
    // ComparisonConfigService, drop any previously loaded entries,
    // and immediately switch to the first group when the config is
    // non-empty.  Status messages from the parser and from the
    // group switch are forwarded through the status reporter.
    // Returns the SwitchGroupResult of the implicit switch_to(0)
    // (defaults to all-false when the config is empty or parse
    // failed) so the caller can refresh the viewport.
    SwitchGroupResult load_comparison_config(const std::string& path);

    // Switch the active comparison group to `group_idx`, releasing
    // the previous group's pixels first.  No-op when group_idx
    // already matches the current index.  Calls load_images() for
    // the resolved local paths, then overrides display labels with
    // the human-friendly titles from the config.  Status messages
    // from the service are forwarded through the status reporter.
    // The returned did_first_load_select mirrors load_images() so
    // the caller knows whether to switch the viewport to Overlay.
    SwitchGroupResult switch_to_comparison_group(int group_idx);

private:
    std::unique_ptr<ImageLibrary> library_;
    std::unique_ptr<SelectionModel> selection_;
    std::unique_ptr<TimelineModel> timeline_;
    std::unique_ptr<DiffService> diff_;
    std::unique_ptr<SrTaskService> sr_tasks_;
    std::unique_ptr<ComparisonConfigService> comparison_config_;
    IStatusReporter* status_reporter_;
    LoaderBackend loader_backend_ = ImageLoader::default_backend();
};

} // namespace idiff

#endif // IDIFF_APP_CONTROLLER_H
