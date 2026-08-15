# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/claude) when working with code in this repository.

## Project Overview

**idiff** is a cross-platform C++17 media comparison tool. It provides split view, A/B overlay, and pixel-level difference heatmap modes with quality metrics (PSNR, SSIM, MSE), plus multi-frame video/timeline support (FFmpeg), channel inspection, pixel inspection, measurement tools, and comparison-group management. It is driven by both the GUI and an external JSON-RPC 2.0 / MCP channel ("single-state, multi-channel"). Built with SDL2 + Dear ImGui; OpenCV for image processing, FFmpeg for video/HEIF/AVIF, ImageMagick and LibRaw as optional backends.

## Build Commands

```bash
# Configure and build (macOS)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(sysctl -n hw.ncpu)

# Linux: replace sysctl with nproc
cmake --build build -j$(nproc)

# Debug build (enables ASAN + UBSAN automatically)
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)

# Run the app
./build/src/app/idiff
./build/src/app/idiff image_a.png image_b.png
./build/src/app/idiff comparison.json
```

### CMake Options

- `IDIFF_WITH_IMAGEMAGICK` — `AUTO` (default) / `ON` / `OFF`. Controls ImageMagick loader backend (Magick++ ≥ 7 via pkg-config).
- `IDIFF_WITH_FFMPEG` — `AUTO` (default) / `ON` / `OFF`. FFmpeg ≥ 8.0 (libavformat ≥ 62.3) via pkg-config; enables video containers, raw YUV, and native HEIF/AVIF decoding.
- `IDIFF_WITH_LIBRAW` — `AUTO` (default) / `ON` / `OFF`. Camera RAW support via LibRaw (system only).
- `ENABLE_SANITIZERS` — ON in Debug, OFF in Release. ASAN + UBSAN.
- `IDIFF_BUILD_TESTS` — ON by default.

## Testing

```bash
# Build and run all tests
cmake --build build --target idiff_tests
cd build && ctest --output-on-failure

# Run a single test by name pattern
./build/tests/idiff_tests "test case name"

# List all test cases
./build/tests/idiff_tests --list-tests
```

Test framework is Catch2 v3. Tests live in `tests/` and mirror the source structure. Some app-layer files are compiled directly into the test executable rather than linked as a library (see `tests/CMakeLists.txt`). Video decoder tests are only compiled when FFmpeg is available; the `ffmpeg` CLI generates the video fixtures at build time, and the tests skip at runtime if it is absent. `idiff_smoke_launch` runs the real binary headless with `--smoke`.

## Architecture

Layered bottom-up design with strict domain/UI separation:

```
idiff_util    Logger (console + rotating file sinks)
idiff_core    Image, ImageLoader, ImageProcessor, ImageComparator, MediaSource
              (ImageFileSource / YuvRawSource / VideoFileSource), MetricsEngine,
              UrlCache, ComparisonConfig, ChannelView, DepthUtils, FileWatcher,
              VideoDecoder + VideoFilterGraph (FFmpeg-gated),
              FFmpegImageLoader + HEIF tile assembler + RAW loader (optional)
idiff_domain  ImageLibrary, SelectionModel, TimelineModel, DiffService,
              ComparisonConfigService, GroupKey/GroupMode, LazyLoadCache
idiff_rpc     JSON-RPC 2.0 dispatcher + Asio transport (UDS on POSIX,
              named pipe on Windows). See docs/rpc-design.md.
idiff (exe)   App, AppController, Viewport, ImGui panels, RPC method
              handlers, platform layer
```

**AppController** (`src/app/controller.h`) is the central seam between domain services and the ImGui GUI. Tests instantiate it with mock collaborators (`IStatusReporter`, `ITextureUploader`, `IFileDialog`).

### Key Patterns

- **PIMPL**: `Image` wraps `cv::Mat` behind opaque `Impl` to hide OpenCV from public headers.
- **Multi-backend loading**: `ImageLoader` selects the still-image backend at runtime via `LoaderBackend` (ImageMagick preferred, OpenCV imgcodecs fallback, FFmpeg for HEIF/AVIF stills ≥ 8.0). Video containers (FFmpeg) and camera RAW (LibRaw) are compile-time gated by `IDIFF_WITH_FFMPEG` / `IDIFF_WITH_LIBRAW` and are not part of `LoaderBackend`.
- **MediaSource abstraction**: `ImageFileSource` (stills), `YuvRawSource` (raw YUV), and `VideoFileSource` (video containers, FFmpeg-gated) share a common multi-frame interface.
- **Lazy diff cache**: `DiffService::mark_dirty()` invalidates; recomputes on `update()`. `LazyLoadCache` (LRU) keeps deselected pixels resident instead of decoding everything at load time.
- **URL cache**: Downloads via system `curl`, caches to disk, background-prefetches adjacent groups; resolves already-unpacked local files before downloading.
- **Single-state, multi-channel**: GUI and the JSON-RPC server mutate the same `AppController` state; RPC handlers run on the main thread via `RpcServer::drain()` each frame. Read `docs/rpc-design.md` before touching `src/app/rpc/` or `tools/idiff-mcp/`; its section 5 documents the MCP tool surface.

## Code Conventions

- Namespace: `idiff`
- C++17, no extensions (`CMAKE_CXX_EXTENSIONS OFF`)
- Include guards (no `#pragma once`)
- PIMPL uses `std::unique_ptr<Impl> impl_`; always null-check before `impl_->` access
- Move operations must be `noexcept`; always test moved-from state
- `compile_commands.json` is exported for LSP tooling
- Third-party deps via FetchContent with system-package fallbacks

## Platform Notes

- **macOS**: Objective-C++ at `src/app/platform/platform_macos.mm`. Build deps via Homebrew (`brew install opencv libraw sdl2 imagemagick ffmpeg`).
- **Linux**: `src/app/platform/platform_posix.cpp`. Deps via apt; note Ubuntu apt ships FFmpeg 6.x, so video support needs a newer FFmpeg (PPA/source) or `IDIFF_WITH_FFMPEG=OFF`.
- **Windows**: `src/app/platform/platform_win32.cpp`. Deps via vcpkg with prebuilt packages from GitHub Releases. The vcpkg manifest includes FFmpeg but not ImageMagick (ImageMagick effectively OFF; OpenCV imgcodecs is the loader). RPC uses named pipes (`\\.\pipe\idiff-<pid>`).

## Related Documentation

- `CODEBUDDY.md` — detailed architecture reference, data flow, and build guide
- `docs/rpc-design.md` — RPC/MCP paradigm, threading model, method reference, roadmap (read before touching `src/app/rpc/`)
- `CHANGELOG.md` — release history

## Known Technical Debt

- `test_logging_integration.cpp` disabled (linking issue with `ImageLibrary::upload`)
- Domain layer links SDL2 because `ImageEntry` stores `SDL_Texture*`
- Some app-layer sources compiled directly into test executable instead of a library target
