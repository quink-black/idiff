---
name: idiffctl
description: |
  Drive the idiff image-comparison GUI from the shell: open images side by
  side, choose the reference, switch split / overlay / difference views,
  read PSNR / SSIM / MSE and pixel values, and save what the viewport
  shows. Use when the user mentions idiff, asks to compare or show images
  in idiff, or asks what an idiff window currently shows.
---

# idiffctl

`idiffctl.py` in this skill directory is the command-line client. It is
executable, so it runs from any working directory as
`<skill-dir>/idiffctl.py`; the examples below write just `idiffctl.py` for
that path. Each command prints one JSON document on stdout; errors go to
stderr. The user sees every change in the idiff window immediately.

```bash
<skill-dir>/idiffctl.py state --brief
```

## Workflow

1. Start from what is on screen:
   - `open A.png B.png ...` loads into the running window, or starts
     idiff when none is running.
   - `state --brief` shows what the user already has open.
2. Arrange: `ref ENTRY`, `select ENTRY...`, `view --mode ...`.
3. Read numbers: `metrics` and `sample X Y`. Answer from these whenever
   the question is numeric; they are exact and cheap.
4. Only when a visual judgement is needed, `shot /tmp/idiff-view.png`
   and then read that image file.

ENTRY is an index or a path. Indices shift after `load` or `rm`, so pass
paths. A relative path is resolved against the current directory.

## Output size

- `state` lists every library entry. Use `state --brief` unless you need
  the full list: it keeps the selection, the reference, the view, and an
  entry count.
- `metrics` and `sample` cover the selection by default; pass entries to
  narrow them.

## Exit status

| Code | Meaning and what to do |
|---|---|
| 0 | success |
| 1 | idiff rejected the request; the stderr message names the field |
| 2 | wrong arguments; check `idiffctl.py COMMAND --help` |
| 3 | no idiff is running; use `open FILE...` |
| 4 | several windows are running; ask the user which one (the window title and status bar show `idiff:<pid>`), then pass `--pid PID` |
| 5 | no answer in time; the request may still have run, so check with `state --brief` before retrying |

## Commands

```bash
idiffctl.py open /data/ref.png /data/out.png    # load, or start idiff
idiffctl.py state --brief
idiffctl.py ref /data/ref.png                   # the "A" side
idiffctl.py select /data/ref.png /data/out.png  # what is compared
idiffctl.py view --mode difference              # split | overlay | difference
idiffctl.py view --mode overlay --slider 0.3
idiffctl.py view --channel y --zoom 4 --pan-x 100 --pan-y 50
idiffctl.py view --group-mode none              # none | by_name | by_folder
idiffctl.py metrics                             # each selected entry vs. the reference
idiffctl.py metrics --ref a.png b.png c.png
idiffctl.py sample 120 80                       # native pixel coordinates
idiffctl.py frame 30                            # video / multi-frame timeline
idiffctl.py shot /tmp/idiff-view.png
idiffctl.py instances
```

`metrics` needs images of identical size and pixel format; a mismatched
image gets an `error` in its result while the others still report.
`psnr` is null and `identical` is true when two images are identical.
`sample` takes coordinates in each image's own pixels, the `width` and
`height` that `state` reports.

Methods without a command go through `call`:

```bash
idiffctl.py call comparison_config.load '{"path": "/data/cmp.json"}'
idiffctl.py call comparison_config.switch_group '{"group_index": 2}'
idiffctl.py call timeline.set_frame_offset '{"path": "/data/b.yuv", "offset": -2}'
idiffctl.py call library.list_comparisons
idiffctl.py call library.set_loader_backend '{"backend": "ffmpeg"}'
```

## Cautions

- idiff computes `metrics`, `sample` and `shot` on its GUI thread; on very
  large images the window pauses until they finish.
- `open --new` always starts another window. Without it, files go into
  the running window, which may already hold the user's own session.
