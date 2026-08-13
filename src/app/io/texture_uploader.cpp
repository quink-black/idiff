#include "app/io/texture_uploader.h"
#include "util/logger.h"

#include <cstring>
#include <limits>

namespace idiff {

SdlTextureUploader::SdlTextureUploader(SDL_Renderer* renderer)
    : renderer_(renderer) {
    SDL_RendererInfo info{};
    if (renderer_ && SDL_GetRendererInfo(renderer_, &info) == 0) {
        limits_.max_width = info.max_texture_width;
        limits_.max_height = info.max_texture_height;
        LOG_INFO("renderer texture limit %dx%d",
                 limits_.max_width, limits_.max_height);
    }
}

SDL_Texture* SdlTextureUploader::upload(const UploadRequest& req) {
    if (!renderer_ || !req.pixels || req.width <= 0 || req.height <= 0 ||
        req.channels != 4) {
        LOG_WARN("texture upload rejected (renderer=%p w=%d h=%d ch=%d)",
                 static_cast<const void*>(renderer_), req.width, req.height,
                 req.channels);
        return nullptr;
    }
    if ((limits_.max_width > 0 && req.width > limits_.max_width) ||
        (limits_.max_height > 0 && req.height > limits_.max_height)) {
        LOG_ERROR("texture dimensions %dx%d exceed renderer limit %dx%d",
                  req.width, req.height,
                  limits_.max_width, limits_.max_height);
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
    SDL_SetTextureScaleMode(
        tex, req.linear_filter ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);

    void* pixels = nullptr;
    int pitch = 0;
    if (SDL_LockTexture(tex, nullptr, &pixels, &pitch) != 0) {
        LOG_ERROR("SDL_LockTexture failed: %s", SDL_GetError());
        SDL_DestroyTexture(tex);
        return nullptr;
    }
    if (pitch < 0) {
        LOG_ERROR("SDL_LockTexture returned negative pitch");
        SDL_UnlockTexture(tex);
        SDL_DestroyTexture(tex);
        return nullptr;
    }

    const std::size_t width = static_cast<std::size_t>(req.width);
    const std::size_t channels = static_cast<std::size_t>(req.channels);
    if (width > std::numeric_limits<std::size_t>::max() / channels) {
        LOG_ERROR("texture row size overflow: %dx%d", req.width, req.channels);
        SDL_UnlockTexture(tex);
        SDL_DestroyTexture(tex);
        return nullptr;
    }
    const std::size_t row_bytes = width * channels;
    const std::size_t src_pitch = req.row_stride ? req.row_stride : row_bytes;
    if (src_pitch < row_bytes) {
        LOG_ERROR("texture source pitch too small: %zu < %zu",
                  src_pitch, row_bytes);
        SDL_UnlockTexture(tex);
        SDL_DestroyTexture(tex);
        return nullptr;
    }
    const std::size_t dst_pitch = static_cast<std::size_t>(pitch);
    if (dst_pitch == row_bytes && src_pitch == row_bytes) {
        const std::size_t height = static_cast<std::size_t>(req.height);
        if (height > std::numeric_limits<std::size_t>::max() / row_bytes) {
            LOG_ERROR("texture byte size overflow: %dx%d",
                      req.width, req.height);
            SDL_UnlockTexture(tex);
            SDL_DestroyTexture(tex);
            return nullptr;
        }
        std::memcpy(pixels, req.pixels,
                    height * row_bytes);
    } else {
        for (int y = 0; y < req.height; ++y) {
            std::memcpy(static_cast<std::uint8_t*>(pixels) + y * pitch,
                        req.pixels + static_cast<std::size_t>(y) * src_pitch,
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
