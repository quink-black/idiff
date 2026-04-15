# idiff

Cross-platform image comparison tool for super resolution workflows.

## Features

- Multi-format image loading (PNG, JPEG, WebP, TIFF, BMP, RAW) with ICC color profile support
- Side-by-side comparison with different-resolution images (auto-upscale with selectable method)
- Visual comparison modes: Split-screen, Overlay/Slider, Difference heatmap
- Quality metrics: PSNR, SSIM, MSE, Histogram
- ICC color profile detection and display
- Plugin system for extensibility

## Build

### Prerequisites

- CMake 3.20+
- C++17 compiler
- OpenCV 4.x (with quality module from opencv_contrib)
- ImageMagick 7.x (with Magick++ C++ bindings)
- LibRaw
- OpenGL 3.3

### macOS

```bash
brew install opencv imagemagick libraw
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(sysctl -n hw.ncpu)
```

### Linux (Ubuntu/Debian)

```bash
sudo apt install libopencv-dev libopencv-contrib-dev libmagick++-dev libraw-dev
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### Windows

Install vcpkg and set up OpenCV, ImageMagick, LibRaw, then:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

## Usage

```bash
# Open with no images
./build/src/app/idiff

# Open with images from command line
./build/src/app/idiff image_a.png image_b.png
```

### Keyboard Shortcuts

- `1/2/3` — Switch comparison mode (Split/Overlay/Difference)
- `+/-` — Zoom in/out
- `Ctrl+O` — Open Image A
- `Ctrl+Shift+O` — Open Image B

## Testing

```bash
cmake --build build --target idiff_tests
cd build && ctest --output-on-failure
```

## License

MIT
