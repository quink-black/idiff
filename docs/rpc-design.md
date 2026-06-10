# idiff RPC + MCP — Design, Status, and Roadmap

> **Audience:** future contributors and AI agents resuming this work,
> potentially on a different machine (notably Windows).
> **Last updated:** 2026-06-10, branch `feature/rpc-phase1`.

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
                  │      │ rpc::RpcServer │  (Asio I/O thread)
                  │      └───────▲───────┘                  │
                  │              │ promise/future per req   │
                  └──────────────┼──────────────────────────┘
                                 │ AF_UNIX SOCK_STREAM
                                 │ 4-byte BE length + JSON
                  ┌──────────────┴──────────────┐
                  │  External clients           │
                  │  - tools/idiff-mcp/         │
                  │    (Python, MCP shim)       │
                  │  - any AF_UNIX client       │
                  └─────────────────────────────┘
```

### Key design choices

- **Threading model.** One Asio I/O thread owns sockets and framing;
  the main GUI thread is the only place handlers run. Hand-off is a
  per-request `std::promise<std::string>` queued behind a mutex.
  `RpcServer::drain()` is called once per `App::frame()` and runs
  every queued handler synchronously on the main thread.
  See `src/app/rpc/rpc_server.{h,cpp}` for the full picture.

- **Wire format.** Plain JSON-RPC 2.0 over a Unix Domain Socket,
  framed with a 4-byte big-endian length prefix followed by UTF-8
  JSON. Frames above **64 MiB** are rejected before allocation. The
  framing is self-contained in `rpc_server.cpp`; the dispatcher in
  `rpc_dispatcher.{h,cpp}` is pure protocol logic with no socket
  awareness, which is what makes it cheap to unit-test.

- **Identity.** Each instance binds `/tmp/idiff-<pid>.sock`. The same
  `<pid>` is shown to the user as `idiff:<pid>` in three places:
  the OS window title, a status-bar chip on the far left, and (via
  cell-label prefixes) on every viewport panel as `[N] filename`.
  Path / label composition lives in
  `src/app/rpc/socket_paths.{h,cpp}`.

- **Stale-socket sweep.** On `App::init()`, we walk `/tmp/idiff-*.sock`
  and probe each via `connect(2)`. Anything that returns
  `ECONNREFUSED` is unlinked (a leftover from a hard kill). Anything
  alive is left alone. This keeps `ls /tmp/idiff-*.sock` an honest
  list of running idiff windows — which the MCP server's
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
| `src/app/rpc/rpc_server.{h,cpp}` | Asio-backed AF_UNIX transport. Owns the I/O thread and the request queue. |
| `src/app/rpc/socket_paths.{h,cpp}` | Path / label composition + stale-socket sweep. POSIX-only today. |
| `src/app/app_rpc_methods.cpp` | All 9 method handlers as `App::register_rpc_methods()`. Member function so handlers reach `App` privates. |
| `src/app/screenshot_composer.{h,cpp}` | Pure renderer: viewport state + entries → `cv::Mat`. Used by both the GUI Save flow and `view.screenshot`. |
| `tests/test_rpc_dispatcher.cpp` | 17 unit tests covering protocol edge cases. |
| `tests/test_rpc_server.cpp` | 4 integration tests (raw AF_UNIX client, no Asio). |
| `tools/idiff-mcp/idiff_client.py` | Python client + discovery (no MCP dep). |
| `tools/idiff-mcp/idiff_mcp_server.py` | MCP shim. 8 tools that map onto idiff RPC. |
| `tools/idiff-mcp/setup.sh` | Provision the local venv, print the `mcp.json` snippet. |
| `tools/idiff-mcp/README.md` | User-facing setup / usage docs. |

CMake gates: `IDIFF_HAVE_RPC` is set in the top-level `CMakeLists.txt`
when standalone-asio is found. Today this is **POSIX only** by
explicit guard — Windows will need work, see below.

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
| `state.get` | none | `{identity, entries:[{index,path,filename,label,width,height,frames}], selection:[int], reference:int|null, explicit_reference:bool, view:{mode,slider}}` |

### Mutations

| Method | Params | Result |
|---|---|---|
| `library.load` | `{paths:[string]}` | `{added:int, total:int}` |
| `library.set_reference` | `{index:int}` | `{}` |
| `library.remove` | `{index:int}` | `{}` |
| `selection.set` | `{indices:[int]}` | `{}` |
| `view.set_mode` | `{mode:"split"|"overlay"|"difference", slider?:float}` | `{}` |
| `view.screenshot` | `{path:string, mode?, slider?}` | `{path,width,height,bytes}` |

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
| `remove_image` | `library.remove` |
| `set_selection` | `selection.set` |
| `set_view_mode` | `view.set_mode` |
| `screenshot` | `view.screenshot` |

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

### Phase 2 — Windows support (NEXT, planned for Windows host)

See "Resuming Work" at the bottom for the actual handoff. Expected
shape:

- [ ] **Transport switch on Windows.** Two options, pick one:
  - **(A) Named pipes via Asio.** Asio supports
    `windows::stream_handle` over named pipes; framing stays the
    same (4-byte BE length + JSON), only the listener / accept
    changes. Path: `\\.\pipe\idiff-<pid>`. Most idiomatic on
    Windows; the user does not need WSL.
  - **(B) AF_UNIX on Windows.** Win10 1803+ supports AF_UNIX
    (`SOCK_STREAM` only). Less code change. Path:
    `%TEMP%\idiff-<pid>.sock`. Limited to recent Windows builds.
  - Recommendation: **(A) named pipes** — wider compatibility,
    `\\.\pipe\` namespace is the canonical Windows IPC location.
    Adds ~150 LoC under `#ifdef _WIN32` in `rpc_server.cpp`.
- [ ] **Stale-server sweep on Windows.** No `/tmp` glob. Use
  `WaitNamedPipe` with a 0 timeout to probe candidate pipes; or
  enumerate via `\\.\pipe\` listing (`FindFirstFileW("\\.\\pipe\\idiff-*")`)
  and `CreateFile` probe. Lives in `socket_paths_win32.cpp`
  paralleling `socket_paths.cpp`.
- [ ] **MCP client.** `idiff_client.py` currently uses
  `socket.AF_UNIX`. On Windows, switch to opening the named pipe
  via `open(path, 'rb+', buffering=0)` (or `pywin32`'s
  `CreateFile`) when `sys.platform == 'win32'`. Discovery uses
  `os.listdir(r'\\.\pipe')` filtered by prefix.
- [ ] **Identity tag.** Window title + status-bar chip already
  cross-platform (no change needed); just confirm the tag survives
  on Windows builds.
- [ ] **Top-level CMake.** Today `IDIFF_HAVE_RPC` is gated on
  `find_path(asio.hpp)` succeeding. On Windows, additionally pull
  in `ws2_32` (for AF_UNIX) or rely on Asio's named-pipe support.
  `target_link_libraries(idiff_rpc PRIVATE ws2_32)` on Windows.
- [ ] **Tests.** Existing `test_rpc_server.cpp` uses `sys/un.h` and
  `connect(2)` directly; either factor a small helper for the
  test client per platform, or skip the integration test on
  Windows and rely on the dispatcher unit tests + manual smoke.
- [ ] **MCP setup script.** `tools/idiff-mcp/setup.sh` is bash-only.
  Add `setup.ps1` mirroring it. Or document
  "use Git Bash / WSL to run setup.sh".

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

### 1. Confirm where Phase 1 left off

```bash
cd /path/to/idiff
git log --oneline | head -10
```

You should see commits `5acb213 .. 766de41` on a branch like
`feature/rpc-phase1`. If they are missing, you are on the wrong
branch — `git fetch && git checkout feature/rpc-phase1`.

```bash
cmake -B build && cmake --build build -j && ctest --test-dir build
```

Expect 303/303 passing on POSIX. On Windows this will not build
yet — that's exactly Phase 2.

### 2. Read the code in this order

1. `docs/rpc-design.md` (this file) — paradigm, status, decisions.
2. `src/app/rpc/rpc_dispatcher.{h,cpp}` — protocol layer, easiest to grok.
3. `src/app/rpc/rpc_server.cpp` — threading model and framing.
4. `src/app/rpc/socket_paths.{h,cpp}` — the place that *will* need
   a Windows companion.
5. `src/app/app_rpc_methods.cpp` — the public API surface.
6. `tools/idiff-mcp/idiff_client.py` — same Windows split applies
   on the Python side.

### 3. Verify the live wire still works (POSIX only today)

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

### 4. Phase 2 starting point

Recommended path for Windows support:

1. **Pick the transport** (named pipes are the recommendation).
2. **Add a transport seam.** Right now `rpc_server.cpp` assumes
   `asio::local::stream_protocol` (AF_UNIX). The cleanest split is
   to introduce a typedef / using directive at the top of
   `rpc_server.cpp` and platform-fork the listener / acceptor /
   per-session reads.
3. **Mirror `socket_paths.cpp` as `socket_paths_win32.cpp`.** Same
   public API (`compose_socket_path`, `compose_identity_label`,
   `sweep_stale_sockets`); `compose_socket_path` returns
   `\\.\pipe\idiff-<pid>` on Windows.
4. **Mirror `idiff_client.py`'s `_probe_socket` / discovery for
   Windows.** Conditional on `sys.platform == 'win32'`.
5. **Test.** The existing `test_rpc_dispatcher.cpp` is portable as
   long as the dispatcher itself stays portable — it should. The
   integration tests in `test_rpc_server.cpp` will need a parallel
   Windows version using `CreateFile` against the pipe.
6. **Smoke run.** Same script as section 3, with the transport
   adjusted.

### 5. Memory / context notes

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
