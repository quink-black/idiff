#!/usr/bin/env python3
"""idiffctl -- drive a running idiff window from the command line.

Every command prints one JSON document on stdout; errors go to stderr.

Exit status:
  0  success
  1  idiff rejected the request (JSON-RPC error)
  2  usage error
  3  no idiff instance is running
  4  several instances are running and none was chosen, or the chosen
     pid is not running
  5  connection failure or timeout

ENTRY arguments name a library entry: a decimal number is an index,
anything else is a path.  Relative paths are made absolute; idiff then
matches the path verbatim against the paths `state` reports or, failing
that, as the same file.

The target instance is --pid, else $IDIFF_PID, else the only running
instance.  Standard library only.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from typing import Any, Optional

sys.path.insert(0, os.path.dirname(os.path.realpath(__file__)))

import idiff_client  # noqa: E402
from idiff_client import (  # noqa: E402
    IdiffClient,
    IdiffConnectionError,
    IdiffRpcError,
    Instance,
)

EXIT_OK = 0
EXIT_RPC = 1
EXIT_USAGE = 2
EXIT_NO_INSTANCE = 3
EXIT_AMBIGUOUS = 4
EXIT_CONNECTION = 5

DEFAULT_TIMEOUT_S = 30.0
DEFAULT_LAUNCH_WAIT_S = 20.0

MODES = ("split", "overlay", "difference")
CHANNELS = ("r", "g", "b", "a", "y", "u", "v", "none", "rgb")
GROUP_MODES = ("none", "by_name", "by_folder")


class CliError(Exception):
    def __init__(self, code: int, message: str):
        super().__init__(message)
        self.code = code


# ---------------------------------------------------------------------
# Argument helpers

def entry_ref(token: str) -> Any:
    return int(token) if token.isdigit() else absolutize(token)


def entry_params(token: str) -> dict:
    ref = entry_ref(token)
    return {"index": ref} if isinstance(ref, int) else {"path": ref}


def absolutize(path: str) -> str:
    # idiff resolves relative paths against its own working directory,
    # which is not the caller's.  URLs pass through unchanged.
    return path if "://" in path else os.path.abspath(path)


def local_paths(paths: list[str]) -> list[str]:
    out = []
    for p in paths:
        a = absolutize(p)
        if "://" not in a and not os.path.exists(a):
            raise CliError(EXIT_USAGE, f"no such file: {a}")
        out.append(a)
    return out


# ---------------------------------------------------------------------
# Instance selection

def pinned_pid(args) -> Optional[int]:
    if args.pid is not None:
        return args.pid
    raw = os.environ.get("IDIFF_PID")
    if not raw:
        return None
    try:
        return int(raw)
    except ValueError:
        raise CliError(EXIT_USAGE, f"IDIFF_PID is not an integer: {raw!r}")


def describe(instances: list[Instance]) -> str:
    return ", ".join(f"pid {i.pid}" for i in instances) or "none"


def choose_instance(args, instances: list[Instance]) -> Optional[Instance]:
    """The instance to target, or None when nothing is running and no
    pid was requested."""
    pid = pinned_pid(args)
    if pid is not None:
        for i in instances:
            if i.pid == pid:
                return i
        raise CliError(EXIT_AMBIGUOUS,
                       f"no idiff instance has pid {pid} "
                       f"(running: {describe(instances)})")
    if len(instances) > 1:
        raise CliError(EXIT_AMBIGUOUS,
                       f"several idiff instances are running "
                       f"({describe(instances)}); pass --pid")
    return instances[0] if instances else None


def require_instance(args) -> Instance:
    inst = choose_instance(args, idiff_client.discover_instances())
    if inst is None:
        raise CliError(EXIT_NO_INSTANCE,
                       "no idiff instance is running "
                       "(start one with: idiffctl open FILE...)")
    return inst


class Session:
    def __init__(self, instance: Instance, timeout: float):
        self.instance = instance
        self._client = IdiffClient(instance.socket_path, timeout=timeout)

    def call(self, method: str, params: Optional[dict] = None) -> Any:
        try:
            return self._client.call(method, params)
        except IdiffRpcError as ex:
            raise CliError(EXIT_RPC, f"{method}: {ex.message}")
        except (IdiffConnectionError, OSError) as ex:
            raise CliError(EXIT_CONNECTION,
                           f"{method}: {self.instance}: {ex}")

    def close(self) -> None:
        self._client.close()


def brief_state(state: dict) -> dict:
    """state.get without the per-entry list, plus the selected entries."""
    entries = state.get("entries", [])
    selection = set(state.get("selection", []))
    out = {k: v for k, v in state.items()
           if k not in ("entries", "comparison_references")}
    out["entry_count"] = len(entries)
    out["selected"] = [
        {k: e.get(k) for k in ("index", "path", "width", "height", "frames")}
        for e in entries if e.get("index") in selection
    ]
    return out


# ---------------------------------------------------------------------
# Launching idiff

def _is_exe(p: str) -> bool:
    return os.path.isfile(p) and os.access(p, os.X_OK)


def find_binary(explicit: Optional[str]) -> Optional[str]:
    """Locate the idiff executable: --bin, $IDIFF_BIN, PATH, the macOS
    Applications folders, then this repository's build tree."""
    for given, origin in ((explicit, "--bin"),
                          (os.environ.get("IDIFF_BIN"), "IDIFF_BIN")):
        if given:
            if not _is_exe(given):
                raise CliError(EXIT_USAGE,
                               f"{origin} is not an executable: {given}")
            return given

    found = shutil.which("idiff")
    if found:
        return found

    candidates = []
    if sys.platform == "darwin":
        for base in ("/Applications", os.path.expanduser("~/Applications")):
            candidates.append(
                os.path.join(base, "idiff.app", "Contents", "MacOS", "idiff"))
    repo = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.realpath(__file__))))
    app_dir = os.path.join(repo, "build", "src", "app")
    candidates += [
        os.path.join(app_dir, "idiff.app", "Contents", "MacOS", "idiff"),
        os.path.join(app_dir, "idiff"),
        os.path.join(app_dir, "Release", "idiff.exe"),
        os.path.join(app_dir, "idiff.exe"),
    ]
    for c in candidates:
        if _is_exe(c):
            return c
    return None


def launch(binary: str, paths: list[str], wait: float) -> Instance:
    """Start idiff detached from this process and wait until it answers.

    The executable is run directly rather than through a launcher such
    as `open -a`, so the child pid is the pid in the socket name.  idiff
    loads files passed on its command line before it serves the first
    request, so a reply means the files are loaded.
    """
    kwargs: dict = dict(stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL, close_fds=True)
    if sys.platform == "win32":
        kwargs["creationflags"] = (subprocess.DETACHED_PROCESS |
                                   subprocess.CREATE_NEW_PROCESS_GROUP)
    else:
        kwargs["start_new_session"] = True
    try:
        proc = subprocess.Popen([binary, *paths], **kwargs)
    except OSError as ex:
        raise CliError(EXIT_CONNECTION, f"cannot start {binary}: {ex}")

    path = idiff_client.socket_path_for_pid(proc.pid)
    deadline = time.monotonic() + wait
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise CliError(EXIT_CONNECTION,
                           f"idiff exited with status {proc.returncode} "
                           f"before it answered on {path}")
        inst = idiff_client.probe_socket(path, timeout=1.0)
        if inst is not None:
            return inst
        time.sleep(0.1)
    raise CliError(EXIT_CONNECTION,
                   f"idiff (pid {proc.pid}) did not answer on {path} "
                   f"within {wait:g} s; it is still running")


# ---------------------------------------------------------------------
# Commands.  Each returns the JSON value to print.

def cmd_instances(args) -> Any:
    instances = idiff_client.discover_instances()
    return {
        "instances": [{"pid": i.pid, "label": i.label,
                       "socket": i.socket_path} for i in instances],
        "pinned_pid": pinned_pid(args),
    }


def cmd_open(args) -> Any:
    paths = local_paths(args.paths)
    inst = None
    if not args.new:
        inst = choose_instance(args, idiff_client.discover_instances())

    if inst is not None:
        s = Session(inst, args.timeout)
        try:
            loaded = s.call("library.load", {"paths": paths})
            state = s.call("state.get")
        finally:
            s.close()
        return {"pid": inst.pid, "socket": inst.socket_path,
                "launched": False, "added": loaded.get("added"),
                "state": brief_state(state)}

    binary = find_binary(args.bin)
    if binary is None:
        raise CliError(EXIT_NO_INSTANCE,
                       "no idiff instance is running and no idiff "
                       "executable was found; pass --bin or set IDIFF_BIN")
    inst = launch(binary, paths, args.wait)
    s = Session(inst, args.timeout)
    try:
        state = s.call("state.get")
    finally:
        s.close()
    return {"pid": inst.pid, "socket": inst.socket_path,
            "launched": True, "state": brief_state(state)}


def cmd_state(s: Session, args) -> Any:
    state = s.call("state.get")
    return brief_state(state) if args.brief else state


def cmd_load(s: Session, args) -> Any:
    return s.call("library.load", {"paths": local_paths(args.paths)})


def cmd_rm(s: Session, args) -> Any:
    return s.call("library.remove", entry_params(args.entry))


def cmd_ref(s: Session, args) -> Any:
    return s.call("library.set_reference", entry_params(args.entry))


def cmd_select(s: Session, args) -> Any:
    forms = [bool(args.entries), args.group is not None,
             args.range is not None, args.clear]
    if sum(forms) != 1:
        raise CliError(EXIT_USAGE,
                       "select: give entries, --group ENTRY, "
                       "--range FROM TO, or --clear")
    if args.group is not None:
        return s.call("selection.select_group", entry_params(args.group))
    if args.range is not None:
        return s.call("selection.select_range",
                      {"from": args.range[0], "to": args.range[1]})
    if args.clear:
        return s.call("selection.set", {"indices": []})
    return s.call("selection.set",
                  {"entries": [entry_ref(e) for e in args.entries]})


def cmd_view(s: Session, args) -> Any:
    applied = []
    if args.group_mode is not None:
        s.call("view.set_group_mode", {"mode": args.group_mode})
        applied.append("view.set_group_mode")
    if args.mode is not None or args.slider is not None:
        mode = args.mode or s.call("state.get")["view"]["mode"]
        params: dict = {"mode": mode}
        if args.slider is not None:
            params["slider"] = args.slider
        s.call("view.set_mode", params)
        applied.append("view.set_mode")
    if args.channel is not None:
        s.call("view.set_channel", {"channel": args.channel})
        applied.append("view.set_channel")
    zoom_pan = {k: v for k, v in (("zoom", args.zoom), ("pan_x", args.pan_x),
                                  ("pan_y", args.pan_y)) if v is not None}
    if zoom_pan:
        s.call("view.set_zoom_pan", zoom_pan)
        applied.append("view.set_zoom_pan")
    if not applied:
        raise CliError(EXIT_USAGE, "view: nothing to change")
    return {"applied": applied}


def cmd_frame(s: Session, args) -> Any:
    return s.call("timeline.set_frame", {"frame": args.frame})


def cmd_metrics(s: Session, args) -> Any:
    params: dict = {}
    if args.ref is not None:
        params["ref"] = entry_ref(args.ref)
    if args.targets:
        params["targets"] = [entry_ref(t) for t in args.targets]
    return s.call("metrics.compare", params)


def cmd_sample(s: Session, args) -> Any:
    params: dict = {"x": args.x, "y": args.y}
    if args.entries:
        params["entries"] = [entry_ref(e) for e in args.entries]
    return s.call("pixel.sample", params)


def cmd_shot(s: Session, args) -> Any:
    params: dict = {"path": absolutize(args.path)}
    if args.mode is not None:
        params["mode"] = args.mode
    if args.slider is not None:
        params["slider"] = args.slider
    return s.call("view.screenshot", params)


def cmd_call(s: Session, args) -> Any:
    params = None
    if args.params is not None:
        try:
            params = json.loads(args.params)
        except json.JSONDecodeError as ex:
            raise CliError(EXIT_USAGE, f"call: params are not JSON: {ex}")
        if not isinstance(params, (dict, list)):
            raise CliError(EXIT_USAGE,
                           "call: params must be a JSON object or array")
    return s.call(args.method, params)


# ---------------------------------------------------------------------
# Parser

def build_parser() -> argparse.ArgumentParser:
    # Global options are accepted before or after the command name.  All
    # copies default to SUPPRESS so neither parser overwrites a value the
    # other one parsed; parse_args() fills in the defaults afterwards.
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--pid", type=int, default=argparse.SUPPRESS,
                        help="target this idiff instance")
    common.add_argument("--timeout", type=float, default=argparse.SUPPRESS,
                        help=f"seconds per request (default "
                             f"{DEFAULT_TIMEOUT_S:g})")
    common.add_argument("--pretty", action="store_true",
                        default=argparse.SUPPRESS, help="indent JSON output")

    p = argparse.ArgumentParser(
        prog="idiffctl",
        description="Drive a running idiff window. Prints JSON.",
        epilog="ENTRY: a decimal number is an index, anything else a path.",
        parents=[common])
    sub = p.add_subparsers(dest="command", required=True, metavar="COMMAND")

    def add(name, help_text, **kw):
        return sub.add_parser(name, help=help_text, description=help_text,
                              parents=[common], **kw)

    add("instances", "list running idiff instances")

    sp = add("open", "load files into idiff, starting it if none is running")
    sp.add_argument("paths", nargs="+", metavar="PATH")
    sp.add_argument("--new", action="store_true",
                    help="always start a new idiff window")
    sp.add_argument("--wait", type=float, default=DEFAULT_LAUNCH_WAIT_S,
                    help=f"seconds to wait for a new window "
                         f"(default {DEFAULT_LAUNCH_WAIT_S:g})")
    sp.add_argument("--bin", help="idiff executable to start")

    sp = add("state", "print the session state")
    sp.add_argument("--brief", action="store_true",
                    help="omit the entry list; list only selected entries")

    sp = add("load", "load files into the library")
    sp.add_argument("paths", nargs="+", metavar="PATH")

    sp = add("rm", "remove an entry from the library")
    sp.add_argument("entry", metavar="ENTRY")

    sp = add("ref", "make an entry the comparison reference")
    sp.add_argument("entry", metavar="ENTRY")

    sp = add("select", "replace the selection")
    sp.add_argument("entries", nargs="*", metavar="ENTRY")
    sp.add_argument("--group", metavar="ENTRY",
                    help="select the comparison group of ENTRY")
    sp.add_argument("--range", nargs=2, type=int, metavar=("FROM", "TO"),
                    help="select indices FROM..TO inclusive")
    sp.add_argument("--clear", action="store_true", help="select nothing")

    sp = add("view", "change how the viewport shows the selection")
    sp.add_argument("--mode", choices=MODES)
    sp.add_argument("--slider", type=float, help="overlay split, 0..1")
    sp.add_argument("--channel", choices=CHANNELS)
    sp.add_argument("--group-mode", choices=GROUP_MODES)
    sp.add_argument("--zoom", type=float, help="1.0 = actual pixels")
    sp.add_argument("--pan-x", type=float, help="screen pixels")
    sp.add_argument("--pan-y", type=float, help="screen pixels")

    sp = add("frame", "jump to a timeline frame")
    sp.add_argument("frame", type=int)

    sp = add("metrics", "PSNR / SSIM / MSE of entries against the reference")
    sp.add_argument("targets", nargs="*", metavar="ENTRY",
                    help="default: the selection without the reference")
    sp.add_argument("--ref", metavar="ENTRY",
                    help="default: the current reference")

    sp = add("sample", "pixel values at native coordinate X Y")
    sp.add_argument("x", type=int)
    sp.add_argument("y", type=int)
    sp.add_argument("entries", nargs="*", metavar="ENTRY",
                    help="default: the reference, then the selection")

    sp = add("shot", "write what the viewport shows to an image file")
    sp.add_argument("path", metavar="PATH", help=".png or .jpg")
    sp.add_argument("--mode", choices=MODES)
    sp.add_argument("--slider", type=float)

    sp = add("call", "send any JSON-RPC method")
    sp.add_argument("method")
    sp.add_argument("params", nargs="?", help="JSON object or array")

    return p


SESSION_COMMANDS = {
    "state": cmd_state, "load": cmd_load, "rm": cmd_rm, "ref": cmd_ref,
    "select": cmd_select, "view": cmd_view, "frame": cmd_frame,
    "metrics": cmd_metrics, "sample": cmd_sample, "shot": cmd_shot,
    "call": cmd_call,
}


def run(args) -> Any:
    if args.command == "instances":
        return cmd_instances(args)
    if args.command == "open":
        return cmd_open(args)
    s = Session(require_instance(args), args.timeout)
    try:
        return SESSION_COMMANDS[args.command](s, args)
    finally:
        s.close()


GLOBAL_DEFAULTS = {"pid": None, "timeout": DEFAULT_TIMEOUT_S, "pretty": False}


def parse_args(argv: Optional[list[str]]) -> argparse.Namespace:
    args = build_parser().parse_args(argv)
    for key, value in GLOBAL_DEFAULTS.items():
        if not hasattr(args, key):
            setattr(args, key, value)
    return args


def main(argv: Optional[list[str]] = None) -> int:
    args = parse_args(argv)
    try:
        result = run(args)
    except CliError as ex:
        print(f"idiffctl: {ex}", file=sys.stderr)
        return ex.code
    if args.pretty:
        text = json.dumps(result, ensure_ascii=False, indent=2)
    else:
        text = json.dumps(result, ensure_ascii=False, separators=(",", ":"))
    print(text)
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
