# idiff RPC + MCP — Design, Status, and Roadmap

> **Audience:** future contributors and AI agents resuming this work,
> potentially on a different machine (notably Windows).
> **Last updated:** 2026-06-10, branch `feature/rpc-phase1`, Phase 2 complete.

This document is the load-bearing reference for the RPC subsystem.
When you (human or agent) sit down to continue this work — especially
on Windows — read this top to bottom, then jump to "Resuming Work" at
the end.

---

## 1. Paradigm: Single-State Multi-Channel

idiff follows what we call the **Single-State Multi-Channel** paradigm:

> One authoritative in-memory state (`App` + `AppController` + the
> domain services it owns), driven by **multiple equally-privileged
> input channels**. The GUI is just one channel; an external agent
> over JSON-RPC is another. Both can read and mutate the same state,
> using the same code paths, with no second source of truth.

Implications that drove every design decision below:

| Implication | Consequence |
|---|---|
| Handlers must touch live App / SDL / OpenCV state | They run on the **main thread** only. The I/O thread never touches App. |
| GUI and RPC must agree on every operation | RPC handlers route through `AppController`; they do not duplicate logic. |
| External clients (CLI, AI agent, MCP) should be on equal footing with the GUI | Wire format is plain JSON-RPC 2.0 over a Unix Domain Socket — anything that can `connect(2)` and frame bytes can drive idiff. |
| Multiple instances coexist | Each idiff binds `/tmp/idiff-<pid>.sock`; the user can tell windows apart from a chip in the status bar. |

Two motivating user scenarios (the original "why"):

1. **Human → AI**: user reaches a buggy state in the GUI, asks the
   agent to introspect it. The agent calls `state.get` instead of
   asking for screenshots.
2. **AI → human**: agent sets up a complex comparison (load N images,
   mark reference, configure mode), hands off to the human for visual
   inspection. The user is already looking at the GUI.

---

## 2. Architecture

```
                  ┌─────────────────────────────────────────┐
                  │        idiff process (single-state)     │
                  │                                         │
   GUI events ────► App / AppController / domain services   │
                  │              ▲                          │
                  │              │ register_method()        │
                  │      ┌───────┴───────┐                  │
                  │      │ rpc::Dispatcher│                 │
                  │      └───────▲───────┘                  │
                  │              │  drain() called every    │
                  │              │  frame() on main thread  │
                  │      ┌───────┴───────┐                  │
                  │      │ rpc::RpcServer │  (Asio I/O      │
                  │      └───────▲───────┘   thread)        │
                  │              │ promise/future per req   │
                  └──────────────┼──────────────────────────┘
                                 │
                    ┌────────────┴────────────┐
                    │ POSIX                    │ Windows                │
                    │ AF_UNIX SOCK_STREAM      │ Named Pipe             │
                    │ /tmp/idiff-<pid>.sock    │ \\.\pipe\idiff-<pid>   │
                    │ 4-byte BE length + JSON  │ 4-byte BE length + JSON│
                    └──────────────────────────┴───────────────────────┘
                  ┌──────────────┐
                  │  External clients           │
                  │  - tools/idiff-mcp/         │
                  │    (Python, MCP shim)       │
                  │  - any socket/pipe client   │
                  └─────────────────────────────┘
```

### Key design choices

- **Threading model.** One Asio I/O thread owns sockets and framing;
  the main GUI thread is the only place handlers run. Hand-off is a
  per-request `std::promise<std::string>` queued behind a mutex.
  `RpcServer::drain()` is called once per `App::frame()` and runs
  every queued handler synchronously on the main thread.
  See `src/app/rpc/rpc_server.{h,cpp}` for the full picture.

- **Wire format.** Plain JSON-RPC 2.0 over a transport (UDS on POSIX,
  named pipe on Windows), framed with a 4-byte big-endian length
  prefix followed by UTF-8 JSON. Frames above **64 MiB** are rejected
  before allocation. The framing is self-contained in
  `rpc_server.cpp`; the dispatcher in `rpc_dispatcher.{h,cpp}` is
  pure protocol logic with no socket awareness, which is what makes it
  cheap to unit-test.

- **Identity.** Each instance binds a transport path:
  POSIX: `/tmp/idiff-<pid>.sock`; Windows: `\\.\pipe\idiff-<pid>`.
  The same `<pid>` is shown to the user as `idiff:<pid>` in three
  places: the OS window title, a status-bar chip on the far left, and
  (via cell-label prefixes) on every viewport panel as
  `[N] filename`.  Path / label composition lives in
  `src/app/rpc/socket_paths.{h,cpp}` (POSIX) and
  `src/app/rpc/socket_paths_win32.cpp` (Windows).

- **Stale-socket sweep.** On `App::init()`, we enumerate existing
  transport paths. On POSIX, we walk `/tmp/idiff-*.sock` and probe
  each via `connect(2)`. Anything that returns `ECONNREFUSED` is
  unlinked (a leftover from a hard kill). Anything alive is left
  alone. On Windows, named pipes are kernel objects that vanish when
  the server process exits, so `FindFirstFileW` enumeration alone is
  sufficient — no probe or unlink is needed. This keeps discovery an
  honest list of running idiff windows — which the MCP server's
  auto-discovery depends on.

- **Why standalone-asio + nlohmann/json.** The user explicitly
  rejected hand-rolled sockets. Both libraries are header-only and
  available in Homebrew / vcpkg.

- **Why JSON-RPC 2.0 (not MCP directly).** Three reasons:
  1. The protocol is small enough to implement from scratch (see the
     17 unit tests in `test_rpc_dispatcher.cpp`).
  2. Decouples idiff from MCP version churn — the MCP shim lives in
     Python and can evolve separately.
  3. Any tool that speaks bytes can drive idiff (CLI, raw `socat`,
     scripts), not just MCP-aware agents.

---

## 3. Components on Disk

| Path | Role |
|---|---|
| `src/app/rpc/rpc_dispatcher.{h,cpp}` | Pure JSON-RPC 2.0 protocol layer (parse, route, error envelopes). Zero socket / threading dependencies. |
| `src/app/rpc/rpc_server.{h,cpp}` | Asio-backed transport (UDS on POSIX, named pipe on Windows). Owns the I/O thread (and accept thread on Windows) and the request queue. |
| `src/app/rpc/socket_paths.{h,cpp}` | Path / label composition + stale-socket sweep (POSIX). |
| `src/app/rpc/socket_paths_win32.cpp` | Windows named-pipe path / label composition + enumeration (no stale cleanup needed). |
| `src/app/app_rpc_methods.cpp` | All 9 method handlers as `App::register_rpc_methods()`. Member function so handlers reach `App` privates. |
| `src/app/screenshot_composer.{h,cpp}` | Pure renderer: viewport state + entries → `cv::Mat`. Used by both the GUI Save flow and `view.screenshot`. |
| `tests/test_rpc_dispatcher.cpp` | 17 unit tests covering protocol edge cases. |
| `tests/test_rpc_server.cpp` | 4 integration tests (platform-abstracted transport client). |
| `tools/idiff-mcp/idiff_client.py` | Python client + discovery (no MCP dep). |
| `tools/idiff-mcp/idiff_mcp_server.py` | MCP shim. 8 tools that map onto idiff RPC. |
| `tools/idiff-mcp/setup.sh` | Provision the local venv, print the `mcp.json` snippet (POSIX). |
| `tools/idiff-mcp/setup.ps1` | Same for Windows (PowerShell). |
| `tools/idiff-mcp/README.md` | User-facing setup / usage docs. |

CMake gates: `IDIFF_HAVE_RPC` is set unconditionally in the top-level
`CMakeLists.txt` (both POSIX and Windows). On Windows, `ws2_32` is
linked and `_WIN32_WINNT=0x0601` is set as a PUBLIC compile definition
on `idiff_rpc`.

---

## 4. RPC Method Reference

All methods live in `src/app/app_rpc_methods.cpp`.

### Identity

| Method | Params | Result |
|---|---|---|
| `app.identity` | none | `{name, pid, socket, label}` |
| `app.list_instances` | none | `{self_pid, self_socket, self_label, instances:[{path,pid,alive,removed,self?,label?}]}` |

### Snapshot

| Method | Params | Result |
|---|---|---|
| `state.get` | none | `{identity, entries:[{index,path,filename,label,width,height,frames}], selection:[int], reference:int\|null, explicit_reference:bool, group_by_name:bool, view:{mode,slider,zoom,pan_x,pan_y,channel_view}, timeline:{current_frame,total_frames}, comparison_references:{key->path}, current_comparison_key:string\|null}` |

### Mutations

| Method | Params | Result |
|---|---|---|
| `library.load` | `{paths:[string]}` | `{added:int, total:int}` |
| `library.set_reference` | `{index:int}` | `{}` |
| `library.list_comparisons` | none | `[{key,name,current,entries:[{index,path,filename,directory,is_reference}],reference_path?}]` |
| `library.set_comparison_reference` | `{key:string, path:string}` | `{}` |
| `library.remove` | `{index:int}` | `{}` |
| `library.reload_all` | none | `{}` — re-decode every entry from disk |
| `library.set_loader_backend` | `{backend:"imagemagick"\|"opencv"\|"ffmpeg"}` | `{backend}` — switch decoder + reload |
| `selection.set` | `{indices:[int]}` | `{}` (rejected with InvalidParams when group-by-name is on and the indices span more than one comparison) |
| `selection.select_group` | `{index:int}` | `{changed:bool, indices:[int]}` |
| `selection.select_range` | `{from:int, to:int}` | `{changed:bool, indices:[int]}` |
| `view.set_mode` | `{mode:"split"\|"overlay"\|"difference", slider?:float}` | `{}` |
| `view.set_group_by_name` | `{enabled:bool}` | `{}` |
| `view.set_zoom_pan` | `{zoom?:float, pan_x?:float, pan_y?:float}` | `{}` — all fields optional |
| `view.set_channel` | `{channel:"r"\|"g"\|"b"\|"a"\|"y"\|"u"\|"v"\|"none"\|"rgb"}` | `{}` |
| `view.screenshot` | `{path:string, mode?, slider?}` | `{path,width,height,bytes}` |
| `comparison_config.load` | `{path:string}` | `{entries:int, groups:int, current_group:int}` |
| `comparison_config.switch_group` | `{group_index:int}` | `{entries:int, current_group:int}` |
| `timeline.set_frame` | `{frame:int}` | `{current_frame:int}` |
| `timeline.set_frame_offset` | `{index:int, offset:int}` | `{index:int, offset:int}` |

### Error policy

- `-32602 InvalidParams` — bad shape, missing field, unknown enum
  value, out-of-range index. Message names the specific field.
- `-32603 InternalError` — unexpected I/O failure (encode failure,
  file write failure). Should be rare in practice.
- The handler validates indices up front so `selection.set` cannot
  apply partially.

---

## 5. MCP Tool Reference

Defined in `tools/idiff-mcp/idiff_mcp_server.py`. Each tool is a thin
shim over one RPC method.

| Tool | Underlying RPC |
|---|---|
| `list_instances` | `app.list_instances` (and re-runs local discovery) |
| `get_state` | `state.get` |
| `load_images` | `library.load` |
| `set_reference` | `library.set_reference` |
| `list_comparisons` | `library.list_comparisons` |
| `set_comparison_reference` | `library.set_comparison_reference` |
| `remove_image` | `library.remove` |
| `set_selection` | `selection.set` |
| `set_view_mode` | `view.set_mode` |
| `set_group_by_name` | `view.set_group_by_name` |
| `screenshot` | `view.screenshot` |
| `set_zoom_pan` | `view.set_zoom_pan` |
| `set_channel_view` | `view.set_channel` |
| `select_group` | `selection.select_group` |
| `select_range` | `selection.select_range` |
| `load_comparison_config` | `comparison_config.load` |
| `switch_comparison_group` | `comparison_config.switch_group` |
| `set_timeline_frame` | `timeline.set_frame` |
| `set_frame_offset` | `timeline.set_frame_offset` |
| `reload_all` | `library.reload_all` |
| `set_loader_backend` | `library.set_loader_backend` |

### Discovery / multi-instance behaviour

| State | MCP behaviour |
|---|---|
| 1 idiff alive | auto-targets it |
| 0 alive | tools return "start idiff first" |
| ≥2 alive (or `IDIFF_PID` points at a non-existent pid) | tools return a structured error listing every live instance with pid + label + socket; the agent should ask the user, then either restart with `IDIFF_PID=<pid>` or use `list_instances` |

The user pin is `IDIFF_PID=<pid>` in the MCP server's environment.
The chip in idiff's status bar shows the matching `idiff:<pid>` so
the user always knows which window the agent is talking to.

---

## 6. Roadmap

### Phase 1 — POSIX RPC + MCP (✅ DONE on `feature/rpc-phase1`)

- [x] `5acb213` Scaffold (idiff_rpc lib + Asio dep + `IDIFF_HAVE_RPC`)
- [x] `aee8b6f` JSON-RPC 2.0 dispatcher + 17 unit tests
- [x] `658c83d` Asio UDS transport + 4 integration tests
- [x] `85e2e2f` Wire server into `App::frame()` lifecycle
- [x] `4ea511b` Extract `compose_viewport()` + `Viewport::set_overlay_slider_pos`
- [x] `f42d17b` 7 Phase-1 method handlers
- [x] `bdcf8e4` Identity + status bar chip + window title + cell-label `[N]` prefix + stale-socket sweep + `app.identity` / `app.list_instances`
- [x] `766de41` Python MCP server with auto-discovery

**Total:** 8 commits, 303/303 tests green, end-to-end MCP smoke test
passing.

### Phase 2 — Windows support (✅ DONE on `feature/rpc-phase1`)

- [x] **Transport switch on Windows.** Option (A): Named pipes via Asio.
  `asio::windows::stream_handle` wraps connected pipe HANDLEs.
  Framing is identical (4-byte BE length + JSON). Path:
  `\\.\pipe\idiff-<pid>`. Accept thread issues `ConnectNamedPipe`
  with OVERLAPPED + `WaitForMultipleObjects` on (pipe-event,
  shutdown-event); posts connected HANDLE to I/O thread which wraps
  it in `stream_handle` and starts the session. Adds +1 background
  thread on Windows (accept thread + Asio I/O thread = 2; POSIX has
  1).
- [x] **Stale-server sweep on Windows.** Named pipes are kernel objects
  that vanish when the server exits. `sweep_stale_sockets()` on
  Windows uses `FindFirstFileW("\\.\pipe\idiff-*")` enumeration;
  `alive` is always `true` for enumerated pipes, `removed` is always
  `false`. No probe or unlink step is needed. Lives in
  `socket_paths_win32.cpp`.
- [x] **MCP client.** `idiff_client.py` uses `ctypes` to call
  `kernel32.CreateFileW` / `ReadFile` / `WriteFile` / `CloseHandle`
  for deterministic byte-mode named-pipe I/O on Windows (Python's
  `open()` on `\\.\pipe\*` has version-dependent quirks). Discovery
  uses `FindFirstFileW` enumeration via `ctypes`. No pywin32
  dependency.
- [x] **Identity tag.** Window title + status-bar chip already
  cross-platform; `::getpid()` replaced with
  `::GetCurrentProcessId()` on Windows.
- [x] **Top-level CMake.** `IDIFF_HAVE_RPC` is now set unconditionally.
  On Windows, `ws2_32` is linked and `_WIN32_WINNT=0x0601` is set
  as a PUBLIC compile definition on `idiff_rpc`.
- [x] **Tests.** `test_rpc_server.cpp` refactored with platform-abstracted
  transport helpers (`connect_transport`, `send_frame`, `recv_frame`,
  etc.) — same test logic on both platforms.
- [x] **MCP setup script.** `tools/idiff-mcp/setup.ps1` mirrors
  `setup.sh` for Windows.

### Phase 3 — Async / events (deferred, no design yet)

- [ ] Long-running operations (e.g. SR inference) should not block
  `drain()`. Move to `task.start` returning a token + `task.poll` /
  `task.wait`.
- [ ] Subscriptions: `events.subscribe` returns a stream of state-
  change notifications. Probably a separate channel (a second pipe
  or a server-streamed JSON-RPC dialect).

### Phase 4 — Coverage extension (deferred, no design yet)

- [ ] `metrics.compute` / `metrics.get` (PSNR, SSIM, MSE).
- [ ] `pixel.sample` / `pixel.histogram` for batch numerical analysis.
- [ ] `comparison_config.load`.
- [ ] `timeline.set_frame` / `timeline.set_offset` for video.

---

## 7. Resuming Work

**You are an agent or human resuming this initiative on a new
machine.** Read this section in order.

### 1. Confirm where Phase 1+2 left off

```bash
cd /path/to/idiff
git log --oneline | head -10
```

You should see the Phase 1 and Phase 2 commits on a branch like
`feature/rpc-phase1`. If they are missing, you are on the wrong
branch — `git fetch && git checkout feature/rpc-phase1`.

```bash
cmake -B build && cmake --build build -j && ctest --test-dir build
```

Expect 303/303 passing on POSIX. On Windows the RPC tests should
also pass now.

### 2. Read the code in this order

1. `docs/rpc-design.md` (this file) — paradigm, status, decisions.
2. `src/app/rpc/rpc_dispatcher.{h,cpp}` — protocol layer, easiest to grok.
3. `src/app/rpc/rpc_server.cpp` — threading model and framing (POSIX + Windows).
4. `src/app/rpc/socket_paths.{h,cpp}` — POSIX path/label/sweep.
5. `src/app/rpc/socket_paths_win32.cpp` — Windows named-pipe path/label/enumeration.
6. `src/app/app_rpc_methods.cpp` — the public API surface.
7. `tools/idiff-mcp/idiff_client.py` — Python client (UDS + named pipe).

### 3. Verify the live wire still works

**POSIX:**

```bash
./build/src/app/idiff.app/Contents/MacOS/idiff &  # or platform equivalent
sleep 2
ls /tmp/idiff-*.sock
# In another shell:
python3 -c "
import socket, struct, json
p = '/tmp/idiff-<pid>.sock'  # fill in
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.connect(p)
body = json.dumps({'jsonrpc':'2.0','method':'app.identity','id':1}).encode()
s.sendall(struct.pack('>I', len(body)) + body)
n = struct.unpack('>I', s.recv(4))[0]; print(s.recv(n).decode())
"
```

**Windows:**

```powershell
# Start idiff.exe, then in PowerShell:
python -c "
from idiff_client import IdiffClient
c = IdiffClient(r'\\.\pipe\idiff-<pid>')  # fill in pid
print(c.call('app.identity'))
c.close()
"
```

### 4. Phase 3 starting point

The user values:

- **Mature cross-platform libraries**, no hand-rolled sockets
  (drove the choice of standalone-asio).
- **Terse, dense responses** — lead with the answer, no closing
  summaries.
- **Single-State Multi-Channel** as the load-bearing concept (this
  document is named after it).

The user's working pattern with idiff is **many open/close cycles
per session, usually one window at a time** — which is why
auto-discovery + on-startup stale sweep is the default and pinning
via `IDIFF_PID` is the escape hatch, not the norm.

### 6. Don't

- Don't expand RPC method scope without flagging it as Phase 3/4 —
  Phase 1 was deliberately constrained.
- Don't add async / events as part of Phase 2; they belong in Phase 3
  with their own design pass.
- Don't break the dispatcher / server split. The dispatcher having
  zero socket dependencies is the property that makes it cheap to
  unit-test and to swap transports.
- Don't replace standalone-asio with anything more exotic; the user
  has already validated this choice.
