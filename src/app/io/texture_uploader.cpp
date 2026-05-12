#include "app/io/texture_uploader.h"
#include "util/logger.h"

#include <cstring>

namespace idiff {

SdlTextureUploader::SdlTextureUploader(SDL_Renderer* renderer)
    : renderer_(renderer) {}

SDL_Texture* SdlTextureUploader::upload(const UploadRequest& req) {
    if (!renderer_ || !req.pixels || req.width <= 0 || req.height <= 0 ||
        req.channels != 4) {
        LOG_WARN("texture upload rejected (renderer=%p w=%d h=%d ch=%d)",
                 static_cast<const void*>(renderer_), req.width, req.height,
                 req.channels);
        return nullptr;
    }

    // Metal does not reliably support SDL_PIXELFORMAT_RGB24 on macOS;
    // we always upload as RGBA32 to keep the path uniform.
    SDL_Texture* tex = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32,
                                         SDL_TEXTUREACCESS_STREAMING,
                                         req.width, req.height);
    if (!tex) {
        LOG_ERROR("SDL_CreateTexture failed: %s", SDL_GetError());
        return nullptr;
    }

    void* pixels = nullptr;
    int pitch = 0;
    if (SDL_LockTexture(tex, nullptr, &pixels, &pitch) != 0) {
        LOG_ERROR("SDL_LockTexture failed: %s", SDL_GetError());
        SDL_DestroyTexture(tex);
        return nullptr;
    }

    const std::size_t row_bytes =
        static_cast<std::size_t>(req.width) * static_cast<std::size_t>(req.channels);
    const std::size_t dst_pitch = static_cast<std::size_t>(pitch);
    if (dst_pitch == row_bytes) {
        std::memcpy(pixels, req.pixels,
                    static_cast<std::size_t>(req.height) * row_bytes);
    } else {
        for (int y = 0; y < req.height; ++y) {
            std::memcpy(static_cast<std::uint8_t*>(pixels) + y * pitch,
                        req.pixels + static_cast<std::size_t>(y) * row_bytes,
                        row_bytes);
        }
    }
    SDL_UnlockTexture(tex);

    LOG_TRACE("uploaded texture %dx%d", req.width, req.height);
    return tex;
}

void SdlTextureUploader::destroy(SDL_Texture* tex) {
    if (tex) SDL_DestroyTexture(tex);
}

} // namespace idiff
