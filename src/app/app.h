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
class Viewport;
class MetricsPanel;
class PropertiesPanel;

struct ImageEntry {
    std::string path;
    std::string filename;
    std::string display_label;
    std::unique_ptr<Image> image;
    std::unique_ptr<Image> display_image;
    SDL_Texture* texture = nullptr;
    int tex_w = 0;
    int tex_h = 0;
    bool texture_dirty = true;
};

struct DiffTexture {
    SDL_Texture* texture = nullptr;
    int tex_w = 0;
    int tex_h = 0;
    bool dirty = true;
};

class App {
public:
    App();
    ~App();

    bool init(SDL_Window* window, SDL_Renderer* renderer);
    void shutdown();
    void frame();

    void load_images(const std::vector<std::string>& paths);

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
    void remove_entry(int index);
    void reload_all_images();
    void update_display_image(int index);
    void upload_texture(ImageEntry& entry);
    void update_diff_texture();
    void upload_diff_texture();
    void compute_display_labels();
    void sort_entries_by_name();
    void move_entry(int from, int to);

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

    std::unique_ptr<Image> diff_image_;
    DiffTexture diff_texture_;
};

} // namespace idiff

#endif // IDIFF_APP_H
