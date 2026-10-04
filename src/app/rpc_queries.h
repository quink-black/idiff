#ifndef IDIFF_APP_RPC_QUERIES_H
#define IDIFF_APP_RPC_QUERIES_H

// Read-only pixel queries behind the metrics.compare and pixel.sample
// RPC methods.  Each function reports a per-image failure inside the
// returned object as {"error": message} instead of throwing, so one
// unusable image does not fail a request that covers several.

#include <nlohmann/json.hpp>

namespace idiff {

class Image;

namespace rpc_queries {

// Full-frame metrics of `target` against `ref`:
// {psnr, ssim, mse, identical}.  psnr is null when the images are
// identical (MSE 0), because JSON cannot carry infinity.  Images that
// differ in dimensions or pixel format yield {error}.
nlohmann::json compare_images(const Image& ref, const Image& target);

// The pixel at native coordinate (x, y):
// {kind, channels, depth, values, text}.  `text` matches the pixel
// inspector panel.  A coordinate outside the image yields {error}.
nlohmann::json sample_pixel(const Image& img, int x, int y);

} // namespace rpc_queries
} // namespace idiff

#endif // IDIFF_APP_RPC_QUERIES_H
