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
    void remove_entry(int index);
    void update_display_image(int index);
    void upload_texture(ImageEntry& entry);
    void update_diff_texture();
    void upload_diff_texture();
    void compute_display_labels();

    struct State;
    std::unique_ptr<State> state_;

    std::vector<ImageEntry> entries_;
    std::set<int> selected_;
    bool first_frame_ = true;

    std::unique_ptr<Image> diff_image_;
    DiffTexture diff_texture_;
};

} // namespace idiff

#endif // IDIFF_APP_H
