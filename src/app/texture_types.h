#ifndef IDIFF_APP_TEXTURE_TYPES_H
#define IDIFF_APP_TEXTURE_TYPES_H

#include <cstddef>
#include <cstdint>

struct SDL_Texture;

namespace idiff {

constexpr int kTextureTileDimension = 2048;

// Source and difference tiles each receive one half of the configured budget.
constexpr std::size_t kGpuTileCachePartitions = 2;

struct TextureTile {
    SDL_Texture* texture = nullptr;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    std::uint64_t last_used_frame = 0;
};

struct VisibleImageRegion {
    int slot = -1;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool difference = false;
};

struct ProjectedTileRect {
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
};

inline ProjectedTileRect project_tile_rect(
        int source_width, int source_height,
        int tile_x, int tile_y, int tile_width, int tile_height,
        float destination_x, float destination_y,
        float destination_width, float destination_height) noexcept {
    if (source_width <= 0 || source_height <= 0) return {};
    const float sx = destination_width / static_cast<float>(source_width);
    const float sy = destination_height / static_cast<float>(source_height);
    return {
        destination_x + tile_x * sx,
        destination_y + tile_y * sy,
        destination_x + (tile_x + tile_width) * sx,
        destination_y + (tile_y + tile_height) * sy,
    };
}

} // namespace idiff

#endif // IDIFF_APP_TEXTURE_TYPES_H
