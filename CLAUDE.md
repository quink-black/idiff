# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**idiff** is a cross-platform C++17 image comparison tool for super-resolution workflows. It provides split view, A/B overlay, and pixel-level difference heatmap modes with quality metrics (PSNR, SSIM, MSE). Built with SDL2 + Dear ImGui, using OpenCV for image processing.

## Build Commands

```bash
# Configure and build (macOS)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(sysctl -n hw.ncpu)

# Debug build (enables ASAN + UBSAN automatically)
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(sysctl -n hw.ncpu)

# Run the app
./build/src/app/idiff
./build/src/app/idiff image_a.png image_b.png
```

### CMake Options

- `IDIFF_WITH_IMAGEMAGICK` — `AUTO` (default) / `ON` / `OFF`. Controls ImageMagick loader backend.
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

Test framework is Catch2 v3. Tests live in `tests/` and mirror the source structure. Some app-layer files are compiled directly into the test executable rather than linked as a library (see `tests/CMakeLists.txt`).

## Architecture

Layered bottom-up design with strict domain/UI separation:

```
idiff_util    Logger (console + rotating file sinks)
idiff_core    Image, ImageLoader, ImageComparator, MediaSource, MetricsEngine,
              UrlCache, ComparisonConfig, ChannelView, DepthUtils
idiff_domain  ImageLibrary, SelectionModel, TimelineModel, DiffService,
              SrTaskService, ComparisonConfigService
idiff (exe)   App, AppController, Viewport, ImGui panels, platform layer
```

**AppController** (`src/app/controller.h`) is the central seam between domain services and the ImGui GUI. Tests instantiate it with mock collaborators (`IStatusReporter`, `ITextureUploader`, `IFileDialog`).

### Key Patterns

- **PIMPL**: `Image` wraps `cv::Mat` behind opaque `Impl` to hide OpenCV from public headers.
- **Multi-backend loading**: `ImageLoader` tries ImageMagick first, falls back to OpenCV imgcodecs. Selectable at runtime via `LoaderBackend`.
- **MediaSource abstraction**: `ImageFileSource` (stills) and `YuvRawSource` (raw YUV) share a common interface.
- **Lazy diff cache**: `DiffService::mark_dirty()` invalidates; recomputes on `update()`.
- **URL cache**: Downloads via system `curl`, caches to disk, background-prefetches adjacent groups.

## Code Conventions

- Namespace: `idiff`
- C++17, no extensions (`CMAKE_CXX_EXTENSIONS OFF`)
- Include guards (no `#pragma once`)
- PIMPL uses `std::unique_ptr<Impl> impl_`; always null-check before `impl_->` access
- Move operations must be `noexcept`; always test moved-from state
- `compile_commands.json` is exported for LSP tooling
- Third-party deps via FetchContent with system-package fallbacks

## Platform Notes

- **macOS**: Objective-C++ at `src/app/platform/platform_macos.mm`. Build deps via Homebrew.
- **Linux**: `src/app/platform/platform_posix.cpp`. Deps via apt.
- **Windows**: `src/app/platform/platform_win32.cpp`. Deps via vcpkg with prebuilt packages from GitHub Releases. ImageMagick is OFF on Windows.

## Related Documentation

- `CODEBUDDY.md` — detailed architecture reference, data flow, and build guide
- `.codebuddy/rules/rules.md` — development rules (feature-done checklist, test standards, interactive feature standards)
- `CHANGELOG.md` — release history

## Known Technical Debt

- `test_logging_integration.cpp` disabled (linking issue with `ImageLibrary::upload`)
- Domain layer links SDL2 because `ImageEntry` stores `SDL_Texture*`
- Some app-layer sources compiled directly into test executable instead of a library target
