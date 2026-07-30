# Group by Folder — Design

## Problem

Current "Group by Name" groups entries by filename stem (e.g.
`input/1.png` and `output/1.png` are one group because both have
stem "1").  This breaks when the comparison group is files within
the **same** directory (e.g. `input/a.png`, `input/b.png`,
`input/c.png`), and same-named files across directories are
unrelated.

## Current State

- `bool group_by_name` in Settings (persisted as
  `panel.group_by_name`)
- Sort: always `filename_less` (stem, ext) — directory-agnostic
- Group key: `group_key_from_filename()` — strips directory,
  returns stem
- UI: single checkbox "Group by Name"
- RPC: `view.set_group_by_name(bool enabled)`

## Design: Replace boolean with GroupMode enum

### New enum

```cpp
enum class GroupMode {
    None,     // flat list, sort by filename only
    ByName,   // current: group by filename stem
    ByFolder, // new: group by parent directory
};
```

### Group key

- `ByName`: `group_key_from_filename(path)` (existing, unchanged)
- `ByFolder`: `group_key_from_directory(path)` (new) — returns the
  parent directory portion of the path, normalized to '/' separators
- `None`: no key, no visual separators

### Sort interaction

- `None`: `sort_by_filename()` (case-insensitive filename)
- `ByName`: `sort_with(filename_less)` (stem, ext) — current
- `ByFolder`: new `sort_by_directory()` — primary key: parent
  directory (case-insensitive), secondary key: filename
  (filename_less)

All three produce contiguous blocks so the visual grouping
separators align with sort boundaries.

### Settings

- Replace `bool group_by_name` with `GroupMode group_mode`
- Backward-compat: old `panel.group_by_name=true` maps to
  `ByName`, `false` maps to `None`
- New key: `panel.group_mode` (values: none|by_name|by_folder)

### UI

Replace checkbox with combo dropdown:
```
[No Grouping v]  ->  No Grouping / Group by Name / Group by Folder
```

### RPC

- New: `view.set_group_mode(mode: "none"|"by_name"|"by_folder")`
- Keep `view.set_group_by_name(bool)` as deprecated alias for
  backward compat (true→by_name, false→none)

### Files to change

1. `domain/group_key.h/.cpp` — add `group_key_from_directory()`
2. `app/settings.h/.cpp` — GroupMode enum, serialization
3. `domain/image_library.h/.cpp` — add `sort_by_directory()`
4. `app/controller.h/.cpp` — sort dispatch, group_indices dispatch
5. `app/app.h/.cpp` — wire GroupMode, replace group_by_name refs
6. `app/ui/image_list.h/.cpp` — combo dropdown, group key dispatch
7. `app/app_rpc_methods.cpp` — new RPC + deprecated alias
8. Tests — group_key_from_directory, sort_by_directory

### Non-goals

- No change to `compute_display_labels()` — it already derives
  shortest distinguishing path suffix, which works for all modes
- No change to selection logic — `on_click_in_group` /
  `on_select_group` already delegate to `group_indices()`, which
  will dispatch on mode
