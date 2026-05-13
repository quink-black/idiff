# Changelog

All notable changes to this project will be documented in this file.

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
