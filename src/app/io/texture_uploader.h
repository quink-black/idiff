#ifndef IDIFF_APP_IO_TEXTURE_UPLOADER_H
#define IDIFF_APP_IO_TEXTURE_UPLOADER_H

// Texture uploader interface.
//
// Encapsulates the SDL_CreateTexture / SDL_LockTexture /
// SDL_UpdateTexture / SDL_DestroyTexture sequence so the rest of the
// app (and its tests) can be written against an injectable contract.
// The return type is still SDL_Texture* because ImGui consumes it
// directly via ImTextureID; replacing that path is not in scope for
// this layer.
//
// Pixel layout contract:
//   * The supplied buffer is always tightly packed in RGBA32 order
//     (R, G, B, A bytes).  The implementation handles row-pitch
//     padding internally; callers should not pre-pad.
//   * width/height must be > 0; channels must be 4.
//   * The returned texture is owned by the caller and must be freed
//     via destroy().  Using SDL_DestroyTexture directly is allowed
//     for now to ease incremental migration.

#include <SDL.h>

#include <cstdint>

namespace idiff {

struct UploadRequest {
    const std::uint8_t* pixels = nullptr;
    int width = 0;
    int height = 0;
    int channels = 4;
};

class ITextureUploader {
public:
    virtual ~ITextureUploader() = default;

    // Allocate a streaming RGBA32 texture, copy `req.pixels` into it,
    // and return ownership.  Returns nullptr on any failure (already
    // logged inside the implementation).
    virtual SDL_Texture* upload(const UploadRequest& req) = 0;

    // Destroy a texture previously returned from upload().  Calling
    // with nullptr is a no-op.
    virtual void destroy(SDL_Texture* tex) = 0;
};

// Default SDL-backed implementation.  Holds a non-owning pointer to
// the SDL renderer; the caller keeps the renderer alive for the life
// of the uploader.
class SdlTextureUploader : public ITextureUploader {
public:
    explicit SdlTextureUploader(SDL_Renderer* renderer);

    SDL_Texture* upload(const UploadRequest& req) override;
    void destroy(SDL_Texture* tex) override;

private:
    SDL_Renderer* renderer_;
};

} // namespace idiff

#endif // IDIFF_APP_IO_TEXTURE_UPLOADER_H
