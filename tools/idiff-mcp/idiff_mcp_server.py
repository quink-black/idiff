"""MCP server exposing the idiff GUI as tools to AI agents.

Bridge from the Model Context Protocol (stdio JSON-RPC over MCP) to the
in-process JSON-RPC 2.0 server every running `idiff` instance hosts on
/tmp/idiff-<pid>.sock.

Discovery model
---------------
Multiple idiff windows are the common case (open / close / re-open
many times in a session).  Each tool call therefore:

  1. Globs /tmp/idiff-*.sock and probes each via app.identity.
  2. If exactly one instance is alive, talks to it.
  3. If zero or more-than-one are alive, returns a structured error
     so the agent can surface the situation to the user and ask which
     PID to target.

The user can pin a specific pid with the IDIFF_PID environment
variable (or per-call by setting it before the agent starts).  Pinning
silences the multi-instance prompt.

Tool surface (mirrors idiff RPC, with friendlier names)
-------------------------------------------------------
  list_instances             -- show every live idiff window
  get_state                  -- snapshot of the active instance (entries, view, ...)
  load_images                -- load files into the GUI library
  set_reference              -- pin one entry as the comparison "A"
  remove_image               -- delete an entry from the library
  set_selection              -- replace the selection
  set_view_mode              -- split | overlay | difference, optional slider
  set_group_by_name          -- toggle the image-list Group-by-Name mode
  screenshot                 -- compose what the viewport currently shows to a file
  list_comparisons           -- enumerate file/config comparisons in the library
  set_comparison_reference   -- record a per-comparison reference path
"""
from __future__ import annotations

import asyncio
import json
import logging
import os
import sys
from typing import Any, Optional

from idiff_client import (
    IdiffClient,
    IdiffConnectionError,
    IdiffRpcError,
    Instance,
    discover_instances,
    select_instance,
)

from mcp.server import Server
from mcp.server.stdio import stdio_server
from mcp.types import TextContent, Tool

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    stream=sys.stderr,
)
log = logging.getLogger("idiff-mcp")

app = Server("idiff")


# ---------------------------------------------------------------------
# Helpers

def _pin_pid() -> Optional[int]:
    """Read IDIFF_PID; return None if unset / not an integer."""
    raw = os.environ.get("IDIFF_PID")
    if not raw:
        return None
    try:
        return int(raw)
    except ValueError:
        log.warning("IDIFF_PID=%r is not an integer; ignoring", raw)
        return None


def _format_instances(instances: list[Instance]) -> str:
    if not instances:
        return "(none running)"
    lines = []
    for i in instances:
        lines.append(f"  - pid={i.pid:<6} label={i.label:<14} socket={i.socket_path}")
    return "\n".join(lines)


def _ambiguous_error(instances: list[Instance], pinned: Optional[int]) -> str:
    """Build the error text shown to the agent when the target is
    ambiguous.  Stays terse so agents can quote it back to the user
    without adding their own framing."""
    if not instances:
        return (
            "No idiff instance is running.\n"
            "Start idiff (and load any files) before invoking idiff MCP "
            "tools."
        )
    if pinned is not None:
        return (
            f"IDIFF_PID={pinned} but no live idiff instance has that pid.\n"
            f"Live instances:\n{_format_instances(instances)}\n"
            f"Either start an idiff with that pid, or unset IDIFF_PID and "
            f"ask the user which instance to target."
        )
    # Multiple alive, no pin.
    return (
        f"Multiple idiff instances are running. "
        f"Ask the user which one to target, then either:\n"
        f"  - export IDIFF_PID=<pid> before launching, or\n"
        f"  - restart the agent with IDIFF_PID set.\n"
        f"Live instances:\n{_format_instances(instances)}"
    )


def _resolve() -> tuple[Optional[Instance], Optional[str], list[Instance]]:
    """Pick the target instance, or return an error message.

    Returns `(instance, error, all_live)`.  Exactly one of `instance`
    and `error` is non-None.  `all_live` is always populated (possibly
    empty) so callers can include it in their response if they want.
    """
    pinned = _pin_pid()
    instance, all_live = select_instance(pinned)
    if instance is None:
        return None, _ambiguous_error(all_live, pinned), all_live
    return instance, None, all_live


def _call(instance: Instance, method: str, params: Optional[dict] = None):
    """Round-trip the underlying RPC, mapping failures to readable text."""
    try:
        with IdiffClient(instance.socket_path) as c:
            return c.call(method, params)
    except IdiffConnectionError as ex:
        raise RuntimeError(
            f"could not reach {instance}: {ex}") from ex
    except IdiffRpcError as ex:
        # Pass the structured error through so the agent sees the
        # specific code (-32602 InvalidParams etc.) and the message
        # the server attached.
        raise RuntimeError(f"{ex} (instance: {instance})") from ex


def _ok(value: Any) -> list[TextContent]:
    """MCP tools return TextContent payloads.  Encode JSON for
    machine-readability and pretty-print for human-readability."""
    text = json.dumps(value, indent=2, ensure_ascii=False)
    return [TextContent(type="text", text=text)]


def _error(text: str) -> list[TextContent]:
    return [TextContent(type="text", text=text)]


# ---------------------------------------------------------------------
# Tool listing

@app.list_tools()
async def list_tools() -> list[Tool]:
    return [
        Tool(
            name="list_instances",
            description=(
                "List every live idiff window currently running on this "
                "machine. Use this when the user mentions multiple idiff "
                "windows, when a tool returned an ambiguous-instance "
                "error, or to confirm IDIFF_PID points at the intended "
                "window. Returns label, pid, and socket path for each."
            ),
            inputSchema={"type": "object", "properties": {}},
        ),
        Tool(
            name="get_state",
            description=(
                "Snapshot the active idiff instance: identity, list of "
                "loaded entries (with width/height/frames), current "
                "selection, explicit reference (if any), view mode, "
                "overlay slider, and the group_by_name flag. Always "
                "start here before issuing any "
                "modifying call -- the entry indices change as the user "
                "loads / removes files."
            ),
            inputSchema={"type": "object", "properties": {}},
        ),
        Tool(
            name="load_images",
            description=(
                "Open one or more image files in idiff. Paths must be "
                "absolute (or anything the OS file dialog would accept). "
                "Same code path as the GUI File > Open: comparison-config "
                "JSONs are auto-detected, raw .yuv files queue the "
                "parameter dialog. Returns the number of entries actually "
                "added and the new total."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "paths": {
                        "type": "array",
                        "items": {"type": "string"},
                        "description": "Files to load (absolute paths preferred)",
                    },
                },
                "required": ["paths"],
            },
        ),
        Tool(
            name="set_reference",
            description=(
                "Mark the entry at the given index as the comparison "
                "reference ('A' side). Adds it to the selection if not "
                "already there. Indices come from get_state.entries[].index. "
                "Also records the choice in the per-comparison "
                "reference map (keyed by the entry's comparison) so "
                "switching away and back to that comparison keeps "
                "this reference."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "index": {"type": "integer", "minimum": 0},
                },
                "required": ["index"],
            },
        ),
        Tool(
            name="list_comparisons",
            description=(
                "Enumerate the comparisons visible to the current "
                "library. A 'comparison' is the set of images shown "
                "together when the user picks one in the Group-by-Name "
                "image list (or the items of the active comparison-"
                "config group). This is the horizontal axis -- which "
                "images appear on screen at once. Each comparison has "
                "a stable 'key' (e.g. 'file:role1' or 'config:My "
                "Group'), a human-readable 'name', a 'current' flag "
                "(entries are loaded), and 'entries' with index, "
                "path, filename, directory, and is_reference. Use "
                "this to inspect file structure before deciding which "
                "entry should be reference per comparison, then call "
                "set_comparison_reference per comparison. Only the "
                "currently-resident comparison-config group has its "
                "entries populated; other config comparisons list "
                "key + name only."
            ),
            inputSchema={"type": "object", "properties": {}},
        ),
        Tool(
            name="set_comparison_reference",
            description=(
                "Pin a specific image path as the reference for one "
                "comparison, keyed by 'key' from list_comparisons. "
                "The mapping persists across comparison switches: "
                "when the comparison becomes active (selection "
                "changes to its members), the entry at this path is "
                "auto-marked as reference. Path is matched exactly "
                "against entry paths; pass an empty path to clear "
                "the mapping. This is the primitive idiff exposes -- "
                "callers implement any rule (by directory, prefix, "
                "regex, ML, ...) and call this per comparison."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "key": {
                        "type": "string",
                        "description": (
                            "Comparison key from list_comparisons[].key"
                        ),
                    },
                    "path": {
                        "type": "string",
                        "description": (
                            "Entry path to pin as reference, or empty "
                            "to clear the mapping for `key`"
                        ),
                    },
                },
                "required": ["key", "path"],
            },
        ),
        Tool(
            name="remove_image",
            description=(
                "Remove the entry at the given index from the library. "
                "Patches the selection automatically. Indices come from "
                "get_state.entries[].index. After removal, indices of "
                "later entries shift down by one -- always re-call "
                "get_state before issuing more index-based calls."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "index": {"type": "integer", "minimum": 0},
                },
                "required": ["index"],
            },
        ),
        Tool(
            name="set_selection",
            description=(
                "Replace the current selection with the given indices. "
                "Empty list clears the selection. Indices must all be "
                "in range -- the call is rejected up front rather than "
                "applied partially."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "indices": {
                        "type": "array",
                        "items": {"type": "integer", "minimum": 0},
                    },
                },
                "required": ["indices"],
            },
        ),
        Tool(
            name="set_view_mode",
            description=(
                "Switch the viewport's comparison mode. 'split' shows "
                "every selected image side-by-side; 'overlay' blends "
                "the first two through an A/B slider; 'difference' "
                "renders one heatmap per partner against the reference. "
                "The optional 'slider' (0..1) is only meaningful in "
                "overlay mode."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "mode": {
                        "type": "string",
                        "enum": ["split", "overlay", "difference"],
                    },
                    "slider": {
                        "type": "number",
                        "minimum": 0.0,
                        "maximum": 1.0,
                    },
                },
                "required": ["mode"],
            },
        ),
        Tool(
            name="set_group_by_name",
            description=(
                "Toggle the image-list 'Group by Name' mode. When on "
                "(the default), images sharing a filename stem form a "
                "single comparison, and set_selection rejects any "
                "selection that spans more than one comparison -- the "
                "same invariant the GUI enforces. Turn it off to build "
                "free-form selections across groups. The current value "
                "is reported by get_state as 'group_by_name'."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "enabled": {"type": "boolean"},
                },
                "required": ["enabled"],
            },
        ),
        Tool(
            name="screenshot",
            description=(
                "Compose the current viewport contents into a single "
                "image and write it to `path`. Optional 'mode' and "
                "'slider' override the GUI state for this snapshot only "
                "(no flicker on screen). Returns the actual file path "
                "written, the composed dimensions, and the encoded byte "
                "size."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "path": {
                        "type": "string",
                        "description": "Output file path (.png or .jpg)",
                    },
                    "mode": {
                        "type": "string",
                        "enum": ["split", "overlay", "difference"],
                    },
                    "slider": {
                        "type": "number",
                        "minimum": 0.0,
                        "maximum": 1.0,
                    },
                },
                "required": ["path"],
            },
        ),
    ]


# ---------------------------------------------------------------------
# Tool dispatch

@app.call_tool()
async def call_tool(
    name: str, arguments: dict[str, Any]
) -> list[TextContent]:

    # list_instances doesn't need a single resolved target -- it
    # always reports every live one.
    if name == "list_instances":
        instances = discover_instances()
        pinned = _pin_pid()
        return _ok({
            "instances": [
                {
                    "pid": i.pid,
                    "label": i.label,
                    "socket": i.socket_path,
                }
                for i in instances
            ],
            "pinned_pid": pinned,
            "count": len(instances),
        })

    instance, err, _all = _resolve()
    if err is not None:
        return _error(err)
    assert instance is not None

    try:
        if name == "get_state":
            return _ok(_call(instance, "state.get"))

        if name == "load_images":
            paths = arguments.get("paths") or []
            if not isinstance(paths, list) or not all(
                    isinstance(p, str) for p in paths):
                return _error("'paths' must be a list of strings")
            return _ok(_call(instance, "library.load", {"paths": paths}))

        if name == "set_reference":
            return _ok(_call(instance, "library.set_reference",
                             {"index": int(arguments["index"])}))

        if name == "list_comparisons":
            return _ok(_call(instance, "library.list_comparisons"))

        if name == "set_comparison_reference":
            return _ok(_call(instance, "library.set_comparison_reference",
                             {"key":  str(arguments["key"]),
                              "path": str(arguments.get("path", ""))}))

        if name == "remove_image":
            return _ok(_call(instance, "library.remove",
                             {"index": int(arguments["index"])}))

        if name == "set_selection":
            indices = arguments.get("indices") or []
            indices = [int(i) for i in indices]
            return _ok(_call(instance, "selection.set",
                             {"indices": indices}))

        if name == "set_view_mode":
            params = {"mode": arguments["mode"]}
            if "slider" in arguments:
                params["slider"] = float(arguments["slider"])
            return _ok(_call(instance, "view.set_mode", params))

        if name == "set_group_by_name":
            return _ok(_call(instance, "view.set_group_by_name",
                             {"enabled": bool(arguments["enabled"])}))

        if name == "screenshot":
            params = {"path": arguments["path"]}
            if "mode" in arguments:
                params["mode"] = arguments["mode"]
            if "slider" in arguments:
                params["slider"] = float(arguments["slider"])
            return _ok(_call(instance, "view.screenshot", params))

        return _error(f"unknown tool: {name}")

    except RuntimeError as ex:
        return _error(str(ex))


# ---------------------------------------------------------------------
# Entrypoint

async def _main_async() -> None:
    async with stdio_server() as (read_stream, write_stream):
        await app.run(
            read_stream,
            write_stream,
            app.create_initialization_options(),
        )


def main() -> None:
    asyncio.run(_main_async())


if __name__ == "__main__":
    main()
