# Changelog

All notable changes to this project will be documented in this file.

## [0.2.2] - 2026-06-04

### Added

- **10-bit and semi-planar YUV raw streams**: Replace the hand-written
  planar YUV-to-RGB decoder with FFmpeg's rawvideo demuxer +
  VideoFilterGraph pipeline. Supports YUV420P10, YUV422P10, YUV444P10,
  P010, and NV16 pixel formats with correct color-space handling
  (BT.601/709/2020 matrix + primaries). The YUV parameter dialog
  exposes color matrix, color primaries, color range, and transfer
  function selectors; all settings are persisted and used to configure
  the filter graph for accurate conversion.
- **Native HEIF and AVIF loading via FFmpeg**: Decode .heic, .heif,
  .hif, and .avif files through libavformat/libavcodec (already linked
  for video) instead of relying on ImageMagick delegates. Multi-tile
  grids are composed via an xstack-based filter graph. No new
  third-party dependency required.
- **Transfer function selector** in the YUV parameter dialog (BT.709,
  PQ, HLG) persisted alongside the existing color metadata fields.

### Changed

- **YUV format names are now FFmpeg strings** (e.g. `"yuv420p"`,
  `"nv12"`) instead of project-local enums. This eliminates the
  translation layer and lets the dialog accept any format that the
  installed FFmpeg build supports, including names not known at compile
  time.
- **LibRaw is now an optional system-detected dependency**. No longer
  fetched from source; found via CMake config or pkg-config, otherwise
  skipped. New `IDIFF_WITH_LIBRAW` CMake option (AUTO/ON/OFF).

### Fixed

- **Windows non-ASCII path crashes**: Embed a UTF-8 process manifest so
  narrow-string Win32 entry points handle Chinese and other non-ASCII
  paths correctly on Windows 10 1903+.
- **Linux CI build failure**: FFmpeg on Ubuntu (6.x) is too old for the
  rawvideo pipeline; the Linux CI job now builds with
  `IDIFF_WITH_FFMPEG=OFF`.

## [0.2.1] - 2026-05-28

### Added

- **Pixel inspector panel**: New Inspector tab samples every selected
  image at a shared coordinate, shows R/G/B (or Y/U/V) values in a
  multi-row table, and supports manual coordinate input. RGB vs RGBA
  samples display a per-channel delta; cross-kind comparisons (e.g.
  RGB vs YUV) are refused instead of printing meaningless numbers.
- **Reload workflow**: Right-click "Reload" and "Reload All" entries
  in the image list, plus a visible Reload button in the toolbar. A
  background file watcher detects on-disk changes and surfaces a
  reload dialog; spurious notifications from background activity are
  suppressed and removed entries are purged from the dialog.
- **Reference image action**: New "Mark as Reference" entry in the
  image list context menu moves a row to the top so it becomes the
  reference for overlay and diff. The selected reference is tagged
  with a single `[Ref]` pill (replacing the previous `[A]`/`[B]`
  labels and the Swap A/B button).
- **Keyframe timeline preview**: Scrubbing the timeline shows a
  thumbnail preview of the target keyframe. The actual seek is now
  deferred until the user releases the drag, so dragging across long
  videos is responsive.
- **vf_scale-based video color pipeline**: Replaced the ad-hoc
  swscale RGB24 path with a reusable libavfilter graph. Color tags
  are read per-AVFrame instead of from codecpar, the source AVFrame
  is attached to the resulting Image so the pixel sampler can read
  values in the original color domain, and the inspector exposes a
  YUV/RGB toggle for video frames.
- **Event-driven file watching on Windows**: Replaces the previous
  polling backend with ReadDirectoryChangesW.
- **Pixel readout labelling**: Status bar prefixes pixel values with
  the channel kind so RGB and YUV readouts are unambiguous.

### Changed

- **Test consolidation**: `ImageComparator`, `MetricsEngine`,
  `ImageProcessor`, `ChannelView`, `depth_utils`, `ImageLoader` UTF-8
  paths, YUV source, video decoder/source, pixel sampler and
  `SeedVR2Engine` pre-launch failures are now grouped into
  parameter-generator-driven test cases so adding new variants no
  longer requires copying boilerplate.
- **Minimum FFmpeg version lowered to 8.0** (libavfilter still
  required for the new color pipeline).

### Fixed

- Fix HLG midtones rendering as washed-out near-white on SDR sinks.
- Fix video reload showing stale content after the source file was
  replaced.
- Fix use-after-free in `FileWatcher` rewatch on both backends.
- Fix inotify backend deadlock from blocking on the wake pipe.
- Fix diff viewport not refreshing after reordering image entries
  (slot textures kept showing pre-drag content).

## [0.2.0] - 2026-05-24

### Added

- **Video file decoding**: New `VideoDecoder` and `VideoFileSource` that
  open container formats (MP4, MKV, MOV, etc.) via FFmpeg, with
  sequential and seek-based frame access. Auto-rotation from the
  container display matrix is applied by default; a manual rotation
  override is also exposed. Optional dependency controlled by
  `IDIFF_WITH_FFMPEG`.
- **Depth utility module**: Extracted duplicated CV_16U-to-CV_8U
  conversion from texture upload, comparator, channel view, and diff
  service into `depth_utils` (`convert_to_8u`, `convert_to_rgba8`).
  Extended coverage to CV_16S and CV_32F so images in those formats
  no longer crash or display incorrectly.
- **RAW 16-bit loading**: Respect the Keep16Bit flag via LibRaw
  `output_bps=16`, so RAW files can now be loaded at full sensor depth.
- **Source bit depth in properties**: New `source_bit_depth` field in
  ImageInfo preserves the original file depth (e.g. 10, 12, 14) as
  reported by ImageMagick, shown in the properties panel and format
  description.
- **CV_32F pixel readout**: Status bar now displays pixel values for
  CV_32F images alongside the existing CV_8U and CV_16U paths.
- **FFmpeg in CI/release workflows**: FFmpeg development libraries and
  CLI are installed on all three platforms; video decoder is built and
  tested in CI. Added FFmpeg to the vcpkg manifest for Windows.
- **LeakSanitizer suppression file**: Suppresses false-positive leaks
  from OpenCL/CUDA driver global singletons so LSan no longer fails
  tests on hosts with GPU drivers.

### Fixed

- Fix 16-bit image rendering regression: 16-bit per-channel images
  (Gray16, RGB16, RGBA16) were passed directly to SDL texture upload
  without downsampling, causing rendering failures.
- Fix histogram for 16-bit images: normalize to 8-bit before
  `cv::calcHist` so pixel values outside [0, 256) are no longer
  silently discarded.

## [0.1.1] - 2026-05-13

### Added

- **Image list context menu**: "Remove Selected" and "Remove All"
  options alongside the existing per-entry "Remove".
- **Selection operations in context menu**: Select All, Select Only
  This, Invert Selection, and Unselect All.
- **ASAN/UBSAN support**: New `ENABLE_SANITIZERS` CMake option,
  enabled by default in Debug builds. Force-builds Catch2 from source
  when active to avoid ABI mismatch.

### Fixed

- Fix missing `<string>` include in timeline_model.h.
- Fix lambda capture in logger test.
- Fix file locking in logger test on Windows.

## [0.1.0] - 2026-05-12

### Added

- **Measurement tool**: Hold M and left-drag to draw measurement
  rectangles in source-image pixel coordinates. Measurements persist
  per-entry across selection changes and can be removed individually
  via the x button or cleared in bulk from the toolbar.
- **Ctrl+left-drag selection zoom**: Trackpad-friendly alternative to
  right-drag for zoom-to-selection on macOS.
- **Structured logger**: Console and rotating file sinks with
  configurable log levels, replacing ad-hoc fprintf diagnostics.

### Changed

- **Diff view respects channel view mode**: When a single channel
  (R, G, B, Alpha, Y, U, V) is selected, the diff heatmap now
  compares only that channel instead of always diffing all RGB
  channels.

### Refactored

- **Architecture overhaul**: The monolithic `App` class has been split
  into a clean domain/UI separation.
  - Extracted domain services: `ImageLibrary`, `SelectionModel`,
    `TimelineModel`, `DiffService`, `SrTaskService`,
    `ComparisonConfigService`.
  - Introduced `AppController` as the service orchestration layer
    between the headless domain and the ImGui front-end.
  - Introduced `IStatusReporter` and `ITextureUploader` /
    `IFileDialog` interfaces so domain services can be tested without
    SDL or NFD.
  - Extracted every UI panel into its own module under `src/app/ui/`
    (toolbar, image list, viewport, inspector, status bar, timeline,
    YUV dialog, error/quit-confirm dialogs).
- **End-to-end controller tests**: Integration tests exercise the
  full load/remove/sort/SR/comparison-config flows through
  `AppController` with fake IO collaborators.
- **Logging coverage tests**: Verify that key domain operations
  produce the expected log output.

## [0.0.8] - 2026-04-28

### Added

- Channel view feature with per-channel (R, G, B) and alpha
  visualization (Gray, Contour).
- RGB channel view mode to drop alpha and show color only.
- Selectable background for RGBA images (solid colors, checkerboard).
- Placeholder display when alpha modes are used on images without
  alpha channel.
- App icon for macOS, Windows, and Linux.

### Changed

- Replaced per-tab View toggles with a single Inspector toggle.
- Scaled checkerboard tile size with image resolution.
- Overhauled README with feature highlights and screenshots.
