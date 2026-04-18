#ifndef IDIFF_APP_H
#define IDIFF_APP_H

#include <memory>
#include <set>
#include <string>
#include <vector>

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;

namespace idiff {

class Image;
class MediaSource;
class Viewport;
class MetricsPanel;
class PropertiesPanel;
struct YuvStreamParams;

struct ImageEntry {
    std::string path;
    std::string filename;
    std::string display_label;
    // Source of pixel data.  For still images, this is an ImageFileSource
    // with frame_count() == 1.  For video streams (e.g. raw YUV) it
    // exposes multiple frames.  The field is always non-null for an
    // entry that was successfully added by load_images().
    std::unique_ptr<MediaSource> source;
    // Cached decoded frame for the current frame index.  This is what
    // all downstream rendering / comparison paths consume.  It is
    // repopulated via source->read_frame() whenever the frame index
    // changes or the loader backend is toggled.
    std::unique_ptr<Image> image;
    std::unique_ptr<Image> display_image;
    SDL_Texture* texture = nullptr;
    int tex_w = 0;
    int tex_h = 0;
    bool texture_dirty = true;

    // Multi-frame bookkeeping (only meaningful when source->frame_count() > 1).
    // frame_offset is a user-tunable per-entry shift applied to the shared
    // timeline index so two streams that start at different frames can be
    // aligned.  cached_frame remembers which frame `image` currently holds;
    // it is used to avoid re-decoding when the effective frame has not
    // changed.
    int frame_offset = 0;
    int cached_frame = 0;
};

// One heatmap comparing A to a specific partner entry.  The partner index
// (into entries_) is kept alongside the texture so the status bar and Save
// flow can map a hovered cell back to "A vs <partner>" without recomputing.
struct DiffSlot {
    int partner_entry_idx = -1;
    std::unique_ptr<Image> image;
    SDL_Texture* texture = nullptr;
    int tex_w = 0;
    int tex_h = 0;
};

class App {
public:
    App();
    ~App();

    bool init(SDL_Window* window, SDL_Renderer* renderer);
    void shutdown();
    void frame();

    void load_images(const std::vector<std::string>& paths);

    // Dispatch entry for any "user asked to open these files" flow
    // (menu, drag & drop, side-bar button, command-line args, ...).
    // Paths with a `.json` extension are routed to the comparison-
    // config loader; everything else is forwarded to load_images().
    // Mixing both kinds in a single call is supported: the first JSON
    // wins and replaces the current session, any other paths are
    // ignored with a status-bar note so the user isn't silently left
    // with a half-loaded state.
    void load_paths(const std::vector<std::string>& paths);

    const std::vector<ImageEntry>& entries() const noexcept { return entries_; }
    const std::set<int>& selected() const noexcept { return selected_; }

private:
    void setup_dock_layout();
    void render_toolbar();
    void render_image_list();
    void render_viewport();
    void render_right_sidebar();
    void render_status_bar();

    void open_file_dialog();
    void save_viewport_dialog();
    // Open a comparison-config JSON file.  Lets the user describe several
    // groups of image URLs in a single document; we then download (with
    // disk caching) one group at a time and feed the cached local paths
    // into load_images().  Decoupling "fetch" from "decode" keeps memory
    // bounded to whatever the current group needs.
    void open_comparison_config_dialog();
    // Parse a comparison-config JSON file from disk and take it over as
    // the current session (tearing down any previously-loaded images
    // and previous config first).  Shared by the dedicated "Open
    // Comparison Config..." menu item and by the generic load_paths()
    // dispatch when the user picks / drops a `.json` file through any
    // other entry point.  On parse error the status bar is updated and
    // the current state is left untouched.
    void load_comparison_config_from_path(const std::string& path);
    // Swap the currently-loaded entries for the contents of
    // comparison_groups_[group_idx].  Images belonging to the previous
    // group are unloaded first so only one group's worth of pixels is
    // resident in memory at a time.  Out-of-range indices are ignored.
    void switch_to_comparison_group(int group_idx);
    void remove_entry(int index);
    void reload_all_images();
    void update_display_image(int index);
    void upload_texture(ImageEntry& entry);
    void update_diff_texture();
    void upload_diff_slot_texture(DiffSlot& slot);
    void compute_display_labels();
    void sort_entries_by_name();
    void move_entry(int from, int to);

    // Pop up the YUV-parameters modal for the next file in
    // pending_yuv_paths_, if any.  Called from frame() after panels so
    // the dialog renders on top.
    void render_yuv_params_dialog();
    // Build a YuvRawSource for the given path+params and append it as a
    // new ImageEntry.  Returns true on success.
    bool add_yuv_entry(const std::string& path, const YuvStreamParams& params);
    // Rebuild the YUV source for an existing entry with new decoder
    // parameters.  Keeps the entry in place (preserving selection, A/B
    // assignment, list order); refreshes cached frame, display label and
    // marks textures / diff dirty.  Returns true on success; on failure
    // the existing source is left intact and a status message is set.
    bool update_yuv_entry_params(int index, const YuvStreamParams& params);
    // Arm the YUV parameters dialog in "edit" mode for the given entry
    // index.  Seeds the dialog with the entry's current parameters so the
    // user can fix a misconfigured stream without reloading.
    void begin_edit_yuv_entry(int index);

    // Timeline bar rendered above the status bar when at least one entry
    // exposes more than one frame.  Returns the bar height in pixels
    // (0 when not drawn) so frame() can shrink the docking area by that
    // amount.
    float render_timeline_bar();
    // Returns the length of the shared timeline, i.e. the maximum number
    // of frames across all multi-frame entries (clamped to at least 1).
    int timeline_length() const;
    // Re-decode frames for every multi-frame entry so they match the
    // current timeline index (plus each entry's per-entry frame_offset).
    // No-op for single-frame entries.  Marks textures and diff dirty on
    // any actual change.
    void sync_entries_to_timeline();

    // Returns the entry indices used as A and B for overlay / diff.
    // Derived from the first two selected items (in selection order),
    // honoring the user-controlled swap flag. Missing slots are -1.
    void get_ab_indices(int& a_idx, int& b_idx) const;

    struct State;
    std::unique_ptr<State> state_;

    std::vector<ImageEntry> entries_;
    std::set<int> selected_;
    bool first_frame_ = true;

    // When true, the A/B assignment derived from `selected_` is swapped.
    // Reset whenever the selection content changes.
    bool swap_ab_ = false;

    // Drag-reorder state for image list
    int drag_source_idx_ = -1;
    int drag_target_idx_ = -1;

    // Maps each slot index passed to Viewport::render (in order) back to
    // the corresponding entries_ index.  Populated by render_viewport() and
    // consumed by render_status_bar() to map the viewport's hover pixel
    // back to a concrete image.
    std::vector<int> viewport_slot_to_entry_;

    // Difference-mode state.  Each slot compares A (the first selected
    // entry, modulo swap_ab_) against one other selected entry.  With N
    // selected entries there are N-1 slots (0 when fewer than 2 selected).
    // The whole vector is treated as a single cache unit invalidated by
    // diff_dirty_; individual slot invalidation is not needed because
    // every trigger (selection change, frame step, decoder reload, ...)
    // forces a full recompute anyway.
    std::vector<DiffSlot> diff_slots_;
    bool diff_dirty_ = true;
};

} // namespace idiff

#endif // IDIFF_APP_H
