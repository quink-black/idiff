#ifndef IDIFF_PROPERTIES_PANEL_H
#define IDIFF_PROPERTIES_PANEL_H

namespace idiff {

class Image;

class PropertiesPanel {
public:
    PropertiesPanel();
    ~PropertiesPanel();

    void render(const Image* image_a, const Image* image_b,
                const Image* display_a = nullptr, const Image* display_b = nullptr);
    void render_inline(const Image* image_a, const Image* image_b,
                       const Image* display_a = nullptr, const Image* display_b = nullptr);

private:
    void render_image_props(const char* label, const Image* img, const Image* display_img);
};

} // namespace idiff

#endif // IDIFF_PROPERTIES_PANEL_H
