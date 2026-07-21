#ifndef IDIFF_APP_CONTROLLER_H
#define IDIFF_APP_CONTROLLER_H

#include "core/image_loader.h"
#include "domain/lazy_load_cache.h"

#include <memory>
#include <set>
#include <string>
#include <unordered_map>
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

    // Returns the reference entry index used by overlay / diff.  When
    // an explicit reference has been set (via mark_as_reference), that
    // index is returned.  Otherwise the smallest selected index is
    // used.  Returns -1 when nothing is selected.  Every other
    // selected entry is a partner compared against the reference.
    void get_ref_index(int& ref_idx) const;

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

    // Return all entry indices that share the same group key as
    // `index`.  Group key = filename stem (everything before the
    // last extension dot).  Returns an empty set when the index is
    // out of range or no matching entries are found.
    std::set<int> group_indices(int index) const;

    // Replace the selection with all entries that share `index`'s
    // group key.  Marks diff dirty and returns true when the
    // selection actually changed.  No-op for out-of-range indices.
    bool select_group(int index);

    // Outcome of click_in_group.  Lets the caller distinguish "we
    // crossed into a different group" from "we toggled a single
    // entry inside the active group" so the UI can update its
    // anchor / texture-dirty bookkeeping accordingly.
    enum class GroupClickAction {
        Noop,      // Out-of-range index; nothing happened.
        Switched,  // Selection was replaced with the clicked group.
        Toggled,   // The single clicked entry was added / removed.
    };

    // Group-aware click handler used by the Image-List panel when
    // "Group by Name" is on.
    //
    //   * If the current selection is empty, or contains any entry
    //     outside `index`'s group, the selection is replaced with
    //     all entries of that group (group switch -- "select all
    //     in the new group").
    //   * Otherwise (every selected entry already belongs to the
    //     same group as `index`) the entry at `index` is toggled
    //     inside the selection so the user can cherry-pick which
    //     images of the active group to compare.
    //
    // Marks diff dirty whenever the selection actually changes.
    GroupClickAction click_in_group(int index);

    // Select all entries in the inclusive range [from, to].
    // Replaces the current selection.  Out-of-range indices are
    // clamped; from > to is swapped.  Returns true if the selection
    // changed.
    bool select_range(int from, int to);

    // Move the entry at `from` to position `to` and patch the
    // selection accordingly.  Out-of-range indices are ignored; a
    // no-op move (from == to) returns immediately.
    void move_entry(int from, int to);

    // Mark the entry at `index` as the reference image: add it to
    // the selection and designate it as the reference so overlay /
    // diff use it as the "A" side.  The entry stays at its current
    // position in the library (no reordering).  Out-of-range indices
    // are ignored.  Also records the choice in the per-comparison
    // reference map so switching away and back to this comparison
    // keeps the same reference (see set_comparison_reference /
    // apply_comparison_reference).
    void mark_as_reference(int index);

    // ---- Lazy-load eviction ----------------------------------------
    //
    // The selection is the authoritative set of "resident" entries.
    // When an entry leaves the selection it is promoted to a small
    // LRU soft cache (LazyLoadCache, N=4) so a brief toggle doesn't
    // force a re-decode; when the LRU overflows the oldest entry's
    // pixels and GPU texture are released via
    // ImageLibrary::release_entry_pixels.
    //
    // on_selection_changed() must be called after every selection
    // mutation -- it diffs the previous selection against the new
    // one, updates the LRU, evicts excess entries, and marks the
    // diff cache dirty so stale partner slots are rebuilt.
    void on_selection_changed();

    // Promote `index` to the front of the LRU without changing the
    // selection.  Used by timeline scrub so the scrubbed entry
    // survives the next eviction sweep while the user is dragging
    // the slider.
    void touch_lazy(int index);

    // ---- Per-comparison reference ---------------------------------
    //
    // A "comparison" is the set of images shown together when the
    // user selects one of them in the Group-by-Name image list (or,
    // with a comparison config loaded, the items of the active
    // config group).  This is the horizontal axis -- which images
    // appear on screen at the same time.
    //
    // The "role" of an image inside a comparison (reference vs
    // candidate, or in general "A vs B vs ...") is the orthogonal
    // vertical axis.  idiff stores only the reference role -- the
    // entry the diff/overlay uses as the "A" side -- and lets
    // external clients decide which entry plays it via any rule
    // they want (directory, prefix, regex, ML, ...).  The rule
    // itself does not live in idiff: callers compute it externally
    // and call set_comparison_reference() per comparison.
    //
    // A comparison key is one of:
    //   "config:<name>"  when a comparison config is active.  <name>
    //                    is the ComparisonGroup::name, or "#<idx>"
    //                    when the config did not provide a name.
    //   "file:<stem>"    otherwise.  <stem> is group_key_from_filename
    //                    of the entry's filename (the same key the
    //                    "Group by Name" UI uses).
    // The "config:" / "file:" prefix names the *origin* of the key,
    // not the concept -- both denote a comparison.

    // One comparison as seen by external clients.
    struct ComparisonView {
        std::string key;
        std::string name;            // human-readable; same as key body
        bool current = false;        // entry indices are loaded
        std::vector<int> entries;    // indices into library_->all()
    };

    // Compute the comparison key for an entry.  Returns an empty
    // string for out-of-range indices.
    std::string comparison_key_of(int index) const;

    // Enumerate the comparisons visible to the current library.
    // When a comparison config is active, returns one ComparisonView
    // per config group (only the currently-resident one has its
    // entries populated; the rest are empty because their pixels are
    // not loaded).  Otherwise returns one ComparisonView per
    // filename-stem comparison.
    std::vector<ComparisonView> list_comparisons() const;

    // Read-only access to the per-comparison reference map.  Keys
    // are comparison keys as above; values are entry paths.
    const std::unordered_map<std::string, std::string>&
    comparison_references() const noexcept { return comparison_reference_; }

    // Pin `path` as the reference for the comparison identified by
    // `key`.  The path is not validated against the current library
    // (the comparison may not be resident); the mapping is consulted
    // lazily by apply_comparison_reference() when the comparison
    // becomes active.  Returns false when key is empty.  Pass an
    // empty path to clear the mapping for `key`.
    bool set_comparison_reference(const std::string& key,
                                  const std::string& path);

    // If a per-comparison reference is recorded for the currently
    // active comparison, and a selected entry has that path, mark
    // it as the selection's explicit reference.  No-op otherwise.
    // Called automatically after comparison switches; exposed for
    // tests.
    void apply_comparison_reference();

    // Remove the entry at `index` (destroying its texture via the
    // injected ITextureUploader), patch the selection, refresh
    // display labels and mark the diff dirty.  Out-of-range indices
    // are ignored.
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

    // Per-comparison reference map.  Keys are comparison keys (see
    // ComparisonView doc above); values are entry paths.  Persisted
    // across comparison switches but not across full sessions.
    // Cleared when a fresh comparison config is loaded.
    std::unordered_map<std::string, std::string> comparison_reference_;

    // LRU of entry indices that recently left the selection.  See
    // on_selection_changed / touch_lazy.
    LazyLoadCache lazy_cache_;

    // Snapshot of the selection at the end of the last call to
    // on_selection_changed().  Used to compute the diff against the
    // current selection so entries that left get inserted into the
    // LRU and entries that entered get removed.
    std::set<int> last_selection_;
};

} // namespace idiff

#endif // IDIFF_APP_CONTROLLER_H
