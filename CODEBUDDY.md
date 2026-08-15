# CODEBUDDY.md

This file provides guidance to CodeBuddy Code when working with code in this repository.

## Build Commands

```bash
# Configure (macOS)
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build -j$(sysctl -n hw.ncpu)

# Build and run all tests
cmake --build build --target idiff_tests
cd build && ctest --output-on-failure

# Run a single test by name
./build/tests/idiff_tests "test case name"

# List all test cases
./build/tests/idiff_tests --list-tests

# Debug build with sanitizers (ASAN + UBSAN enabled by default in Debug)
cmake -B build -DCMAKE_BUILD_TYPE=Debug
```

### CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `IDIFF_WITH_IMAGEMAGICK` | `AUTO` | `AUTO`/`ON`/`OFF` — ImageMagick loader backend (Magick++ ≥ 7) |
| `IDIFF_WITH_FFMPEG` | `AUTO` | `AUTO`/`ON`/`OFF` — FFmpeg ≥ 8.0 video decoder + native HEIF/AVIF |
| `IDIFF_WITH_LIBRAW` | `AUTO` | `AUTO`/`ON`/`OFF` — LibRaw camera RAW decoder |
| `ENABLE_SANITIZERS` | ON for Debug, OFF for Release | AddressSanitizer + UndefinedBehaviorSanitizer |
| `IDIFF_BUILD_TESTS` | `ON` | Build the Catch2 test suite |

## Architecture

Layered C++17 GUI application. SDL2 + Dear ImGui for rendering and UI.

### Library Targets (bottom-up)

```
idiff_util   -> Logger (console + rotating file sinks)
idiff_core   -> Image, ImageLoader, ImageProcessor, ImageComparator,
                MediaSource (ImageFileSource / YuvRawSource /
                VideoFileSource), MetricsEngine, UrlCache,
                ComparisonConfig, ChannelView, DepthUtils, FileWatcher,
                VideoDecoder / VideoFilterGraph (FFmpeg-gated),
                FFmpegImageLoader / HEIF tile assembler (FFmpeg-gated),
                RAW loader (LibRaw-gated)
idiff_domain -> ImageLibrary, SelectionModel, TimelineModel, DiffService,
                ComparisonConfigService, GroupKey / GroupMode,
                LazyLoadCache (LRU pixel cache, header-only)
idiff_rpc    -> JSON-RPC 2.0 dispatcher + Asio transport (UDS on POSIX,
                named pipe on Windows; IDIFF_HAVE_RPC is set unconditionally).
                See docs/rpc-design.md.
idiff        -> GUI executable (App, AppController, Viewport, ImGui panels,
                RPC method handlers)
idiff_tests  -> Catch2 test suite
```

### RPC + MCP

idiff exposes a JSON-RPC 2.0 server (`/tmp/idiff-<pid>.sock` on POSIX,
`\\.\pipe\idiff-<pid>` on Windows) so
external clients (CLI, AI agents, the bundled MCP shim in
`tools/idiff-mcp/`) can drive the same `App` state the GUI does.
**Read `docs/rpc-design.md` before touching anything under `src/app/rpc/`,
`src/app/app_rpc_methods.cpp`, or `tools/idiff-mcp/`.** That document
covers the paradigm, threading model, current status, and the
Phase 2 (Windows) handoff.

### Domain/UI Separation

`AppController` (`src/app/controller.h`) is the seam. It owns all domain services and orchestrates business logic without UI dependencies. The `App` class (ImGui shell) delegates to `AppController` for non-UI work. Tests instantiate `AppController` alone with fake collaborators.

### Interface Seams for Testability

- `IStatusReporter` — abstract status bar / error dialog
- `ITextureUploader` — abstract SDL texture upload
- `IFileDialog` — abstract native file dialog (NFD)

### Key Patterns

- **PIMPL**: `Image` wraps `cv::Mat` behind an opaque impl to hide OpenCV from the public API.
- **Multi-backend loading**: `ImageLoader` selects the still-image backend at runtime via `LoaderBackend` (ImageMagick preferred for ICC profiles and wide format coverage, OpenCV imgcodecs fallback, FFmpeg for HEIF/AVIF stills ≥ 8.0). Video containers (FFmpeg) and camera RAW (LibRaw) are compile-time gated by `IDIFF_WITH_FFMPEG` / `IDIFF_WITH_LIBRAW` and are not part of `LoaderBackend`.
- **MediaSource abstraction**: `ImageFileSource` (stills), `YuvRawSource` (raw YUV), and `VideoFileSource` (video containers; the latter two are FFmpeg-gated) share a common interface for multi-frame support.
- **Lazy diff cache**: `DiffService` invalidates via `mark_dirty()` and recomputes on next `update()`. `LazyLoadCache` (LRU) keeps recently deselected pixels resident instead of decoding every frame at load time.
- **URL cache**: `UrlCache` downloads via system `curl`, caches to disk, background-prefetches adjacent comparison groups, and resolves already-unpacked local files before downloading.
- **Single-state, multi-channel**: GUI and the JSON-RPC server mutate the same `AppController` state; RPC handlers run on the main thread via `RpcServer::drain()` once per frame. See `docs/rpc-design.md`.

### Data Flow (Core Comparison)

```
File path / URL
  -> ImageLoader (ImageMagick / OpenCV / FFmpeg)
  -> Image (cv::Mat wrapper)
  -> MediaSource (ImageFileSource, YuvRawSource, or VideoFileSource)
  -> ImageEntry (source + cached Image + SDL_Texture)
  -> ImageLibrary (owns entry collection)
  -> SelectionModel (A/B selection + swap toggle)
  -> DiffService (lazy heatmap recomputation)
  -> Viewport (Split/Overlay/Difference rendering)
```

### Known Issues

- `test_logging_integration.cpp` is disabled in `tests/CMakeLists.txt` due to a linking issue with `ImageLibrary::upload`.
- The domain layer still links SDL2 because `ImageEntry` stores `SDL_Texture*` — acknowledged as future cleanup.

## Testing

Catch2 v3 with `catch_discover_tests()`. Tests live in `tests/` and mirror the source structure. Integration tests exercise `AppController` with mock `ITextureUploader` and `IStatusReporter`.

Some app-layer source files (controller, IO adapters) are compiled directly into the test executable rather than linked as a library — see `tests/CMakeLists.txt`.

## Platform Notes

- macOS: Objective-C++ platform layer at `src/app/platform/platform_macos.mm`
- Linux: `src/app/platform/platform_posix.cpp`
- Windows: `src/app/platform/platform_win32.cpp`, vcpkg-based dependency management
- Distribution: `scripts/macos_bundle_deps.sh` bundles dylibs into `.app`; CI produces `.dmg`/`.zip` (macOS) and `.zip` (Windows)

## Project Conventions

- Namespace: `idiff`
- C++17, no extensions (`CMAKE_CXX_EXTENSIONS OFF`)
- `compile_commands.json` is exported for tooling
- Third-party dependencies via FetchContent (imgui, Catch2, nfd, nlohmann_json, asio) with system-package fallbacks; LibRaw, ImageMagick, and FFmpeg are detected on the system only
