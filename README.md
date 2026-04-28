# idiff

Cross-platform image comparison tool for super-resolution workflows.

![img.png](resource/img.png)

## Features

### Comparison Modes

- **Split** — side-by-side grid, auto-upscales different-resolution images (Lanczos / Nearest / Bilinear / Area)
- **Overlay** — A/B slider with drag handle
- **Difference** — pixel-level heatmap with adjustable amplification and color scheme (Inferno / Viridis / Magma / Turbo / Coolwarm)

![overlay](resource/overlay.png)

![diff](resource/diff.png)

### Quality Metrics

- PSNR, SSIM, MSE per-pair and per-image
- Per-channel statistics (mean, stddev, min, max, variance)
- Histogram visualization
- Cached computation — flip tabs without recomputation

### Image Support

- **Formats**: PNG, JPEG, WebP, TIFF, BMP, RAW (via LibRaw), HEIF/AVIF (via ImageMagick)
- **ICC color profile** detection and display
- **Raw YUV video streams** — configurable resolution, pixel format, frame stepping
- **HTTP/HTTPS URLs** — automatic download with on-disk cache and background prefetch
- **Drag-and-drop** files or comparison-config JSON into the window

### Channel Inspection

- R, G, B single-channel views
- Alpha channel (grayscale or contour)
- YUV planar channels (Y, U, V)
- RGB composite (drop alpha)

![alpha](resource/alpha.png)

![alpha-gray](resource/alpha-gray.png)

### Viewport Tools

- **Ruler** — pixel-coordinate rulers anchored to cell edges
- **Grid overlay** — configurable columns / rows
- **Zoom** — fit, 1:1, scroll-wheel, or toolbar buttons
- **Pan** — click-drag to pan
- **Save viewport** — export current Split / Overlay / Diff view to PNG or JPEG at full image resolution

![grid](resource/grid.png)

### Comparison Config

Load a JSON file to batch-compare groups of images (local paths or URLs):

```json
{
  "groups": [
    {
      "name": "Model A vs B",
      "images": [
        { "url": "https://example.com/ground_truth.png", "title": "GT" },
        { "url": "https://example.com/model_a.png", "title": "Model A" },
        { "url": "https://example.com/model_b.png", "title": "Model B" }
      ]
    }
  ]
}
```

Open via `File > Open Comparison Config...` or drag the JSON into the window. Adjacent groups are prefetched in the background.

## Build

### Prerequisites

- CMake 3.20+
- C++17 compiler
- OpenCV 4.x (with imgcodecs and quality module from opencv_contrib)
- LibRaw
- SDL2
- ImageMagick 7+ (optional — preferred loader for ICC profiles and wider format coverage)
- vcpkg (recommended on Windows)

### macOS

```bash
brew install opencv libraw sdl2 imagemagick
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(sysctl -n hw.ncpu)
```

### Linux (Ubuntu/Debian)

```bash
sudo apt install libopencv-dev libopencv-contrib-dev libraw-dev libsdl2-dev libmagick++-dev
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### Windows (vcpkg)

```cmd
set VCPKG_ROOT=C:\path\to\vcpkg
cmake -B build -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

## Usage

```bash
# Open with no images
./build/src/app/idiff

# Open with images from command line
./build/src/app/idiff image_a.png image_b.png

# Open a comparison config
./build/src/app/idiff comparison.json
```

### Keyboard Shortcuts

| Shortcut | Action |
|---|---|
| `Ctrl+O` | Open images |
| `Ctrl+S` | Save viewport |
| `0` / `F` | Fit to content |
| `1`–`9` | Channel view (1=all, 2=RGB, 3=R, 4=G, 5=B, 6=Alpha, 7=Alpha contour, 8=Y, 9=U) |
| `Esc` | Cancel selection |
| Double-click | Fit to content |

Mouse: scroll to zoom, drag to pan.

## Testing

```bash
cmake --build build --target idiff_tests
cd build && ctest --output-on-failure
```

## License

MIT
