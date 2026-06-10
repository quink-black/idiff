# idiff-mcp

MCP server bridging the [idiff](../../) GUI to AI agents.

idiff hosts a JSON-RPC 2.0 server inside every running window, on
`/tmp/idiff-<pid>.sock`. This MCP server discovers those sockets,
picks the right one, and exposes idiff's RPC methods as MCP tools so
an agent (Codebuddy / Claude Desktop / Cursor / ...) can drive idiff
from a conversation.

## Setup

```bash
cd tools/idiff-mcp
bash setup.sh
```

The script provisions a local `.venv/`, installs `mcp>=1.0.0`, and
prints a snippet you can paste into `~/.codebuddy/mcp.json`.

## Discovery model

- **One idiff window running** &rarr; auto-targeted, no config needed.
- **Multiple idiff windows running** &rarr; every tool call returns a
  structured error listing the live instances. The agent should ask the
  user which one to target, then either restart with `IDIFF_PID=<pid>`
  in the environment, or call `list_instances` first to confirm.
- **No idiff running** &rarr; tools return a "start idiff first"
  message.

idiff's status bar shows `idiff:<pid>` on the left and each viewport
cell is labelled `[N] filename` so the user has a stable identifier
they can quote when the agent prompts them.

## Tools

| Tool | What it does |
|---|---|
| `list_instances` | Enumerate every live idiff window |
| `get_state` | Snapshot identity + entries + selection + view mode |
| `load_images` | Open files in idiff (same code path as File > Open) |
| `set_reference` | Pin one entry as the comparison "A" side |
| `remove_image` | Delete an entry from the library |
| `set_selection` | Replace the selection wholesale |
| `set_view_mode` | Switch split / overlay / difference (+ optional slider) |
| `screenshot` | Compose the viewport contents to a file |

## Pinning a specific instance

```bash
IDIFF_PID=12345 codebuddy ...
```

The MCP server reads `IDIFF_PID` once per call. Pinning silences the
multi-instance prompt and refuses to fall back to any other window.

## Implementation notes

- `idiff_client.py` holds the raw RPC client and discovery logic.
  Stand-alone -- no MCP dependency. Reusable from ad-hoc scripts.
- `idiff_mcp_server.py` is the MCP shim. Each MCP tool maps 1-to-1 to
  one idiff RPC method, with friendlier names and inline schemas so
  the agent picks the right one.
- Tools open a fresh socket per call (cheap on a Unix domain socket).
  This makes the server stateless and survives the user closing /
  reopening idiff between calls.
