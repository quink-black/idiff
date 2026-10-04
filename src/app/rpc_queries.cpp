#include "app/rpc_queries.h"

#include "app/pixel_sampler.h"
#include "core/image.h"
#include "core/metrics_engine.h"

#include <opencv2/core.hpp>

#include <cmath>
#include <string>

namespace idiff {
namespace rpc_queries {

using nlohmann::json;

namespace {

const char* kind_name(PixelKind k) {
    switch (k) {
        case PixelKind::Gray:    return "gray";
        case PixelKind::RGB:     return "rgb";
        case PixelKind::YUV:     return "yuv";
        case PixelKind::Unknown: break;
    }
    return "unknown";
}

json depth_name(int depth) {
    switch (depth) {
        case CV_8U:  return "8u";
        case CV_16U: return "16u";
        case CV_32F: return "32f";
        default:     return depth;
    }
}

} // namespace

json compare_images(const Image& ref, const Image& target) {
    MetricsEngine engine;
    auto r = engine.compute(ref, target);
    if (!r) return json{{"error", engine.last_error()}};

    const bool identical = std::isinf(r->psnr);
    return json{
        {"psnr",      identical ? json(nullptr) : json(r->psnr)},
        {"ssim",      r->ssim},
        {"mse",       r->mse},
        {"identical", identical},
    };
}

json sample_pixel(const Image& img, int x, int y) {
    const auto& info = img.info();
    if (x < 0 || y < 0 || x >= info.width || y >= info.height) {
        return json{{"error",
            "(" + std::to_string(x) + ", " + std::to_string(y) +
            ") is outside the " + std::to_string(info.width) + "x" +
            std::to_string(info.height) + " image"}};
    }

    PixelSample s = sample_image_at(&img, pixel_to_norm(x, info.width),
                                    pixel_to_norm(y, info.height));
    if (!s.valid) return json{{"error", "image has no pixel data"}};

    const bool integral = s.depth == CV_8U || s.depth == CV_16U;
    json values = json::array();
    for (int i = 0; i < s.channels; ++i) {
        if (integral) values.push_back(std::lround(s.v[i]));
        else          values.push_back(s.v[i]);
    }
    char text[128];
    format_pixel(s, text, sizeof(text));

    return json{
        {"kind",     kind_name(s.kind)},
        {"channels", s.channels},
        {"depth",    depth_name(s.depth)},
        {"values",   std::move(values)},
        {"text",     text},
    };
}

} // namespace rpc_queries
} // namespace idiff
