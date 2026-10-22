"""Command-line entry point: one JSON document on stdout, errors on stderr."""

from __future__ import annotations

import argparse
import json
import shutil
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional

from . import loader
from .adapter import Adapter, Manifest
from .context import Context, cli_command
from .errors import BridgeError
from .loader import adapters_root, available, load_adapter_by_name
from .session import Host
from .settings import load_settings, settings_path, config_root


def _emit(doc: Any) -> None:
    json.dump(doc, sys.stdout, ensure_ascii=False)
    sys.stdout.write("\n")


def _host(args: argparse.Namespace) -> Host:
    """The Host commands use; tests inject one through main(host=...)."""
    return getattr(args, "host", None) or Host()


def _ctx(adapter: Adapter, manifest: Manifest, query: Optional[dict] = None,
         host: Optional[Host] = None) -> Context:
    schema = manifest.settings or adapter.settings_schema()
    return Context(adapter=adapter.name, host=host or Host(),
                   settings=load_settings(adapter.name, schema),
                   query=query or {},
                   schema=schema,
                   buttons=list(manifest.buttons))


def cmd_adapters(args: argparse.Namespace) -> Any:
    found = available()
    out = []
    for name, directory in found.items():
        try:
            manifest = loader.read_manifest(directory)
            actions = sorted(loader.load_adapter(directory).actions())
            out.append({"name": name, "path": str(directory),
                        "actions": actions,
                        "settings": sorted(manifest.settings)})
        except BridgeError as e:
            out.append({"name": name, "path": str(directory),
                        "error": str(e)})
    return {"adapters": out, "root": str(adapters_root())}


def cmd_run(args: argparse.Namespace) -> Any:
    adapter = load_adapter_by_name(args.adapter)
    manifest = loader.read_manifest(available()[args.adapter])
    specs = adapter.actions()
    if args.action not in specs:
        raise BridgeError(
            f"adapter {args.adapter!r} has no action {args.action!r}; "
            "known: " + ", ".join(sorted(specs)))
    raw: Dict[str, str] = {}
    for pair in args.param or []:
        key, sep, value = pair.partition("=")
        if not sep or not key:
            raise BridgeError(f"--param expects key=value, got {pair!r}")
        raw[key] = value
    kwargs = loader.bind_params(specs[args.action], manifest, raw)
    ctx = _ctx(adapter, manifest, host=_host(args))
    result = specs[args.action].func(adapter, ctx, **kwargs)
    return {"ok": True, "adapter": args.adapter, "action": args.action,
            "result": result}


def cmd_url(args: argparse.Namespace) -> Any:
    from . import urlhandler
    return urlhandler.open_link(args.link, host=getattr(args, "host", None))


def cmd_current(args: argparse.Namespace) -> Any:
    return _host(args).current_group()


def cmd_advance(args: argparse.Namespace) -> Any:
    result = _host(args).advance(args.step)
    if result is None:
        return {"ok": False, "reason": "there is no further group"}
    return result


def cmd_new(args: argparse.Namespace) -> Any:
    target = adapters_root() / args.name
    if target.exists():
        raise BridgeError(f"{target} already exists")
    template = Path(__file__).resolve().parent.parent / "template"
    if not template.is_dir():
        raise BridgeError(f"{template}: template directory is missing")
    shutil.copytree(template, target)
    for path in target.iterdir():
        if path.is_file():
            path.write_text(path.read_text(encoding="utf-8").replace(
                "__NAME__", args.name), encoding="utf-8")
    return {"created": str(target),
            "next": [f"edit {target}/adapter.toml and adapter.py",
                     "run: idiff-bridge doctor " + args.name,
                     f"test: python3 -m unittest discover -s {target}"]}


def cmd_doctor(args: argparse.Namespace) -> Any:
    """Reports what a new install is missing, one check per line."""
    checks: List[Dict[str, Any]] = []

    def add(name: str, ok: bool, detail: str = "",
            fix: str = "") -> None:
        checks.append({"check": name, "ok": ok, "detail": detail,
                       "fix": "" if ok else fix})

    found = available()
    add("adapter", args.name in found,
        str(found.get(args.name, "")),
        f"create it with: idiff-bridge new {args.name}")
    if args.name in found:
        try:
            adapter = loader.load_adapter(found[args.name])
            add("actions", True, ", ".join(sorted(adapter.actions())))
        except BridgeError as e:
            add("actions", False, str(e), "fix adapter.py or adapter.toml")
            adapter = None
        if adapter is not None:
            from .loader import read_manifest
            schema = read_manifest(found[args.name]).settings
            if not schema:
                schema = adapter.settings_schema()
            path = settings_path(args.name)
            missing = []
            values = {}
            try:
                values = load_settings(args.name, schema, require=False)
                missing = [k for k, s in schema.items()
                           if s.get("required") and not values.get(k)]
            except BridgeError as e:
                add("settings", False, str(e), f"edit {path}")
            required = [k for k, s in schema.items() if s.get("required")]
            # "ok" means every required setting is present; a settings file
            # with only optional values still needs the required ones.
            ok = not missing and (bool(values) or not required)
            add("settings", ok, f"{path} ({len(values)} value(s))",
                "set: " + ", ".join(missing or required or ["(no values)"])
                + f" in {path}")
            if values.get("server"):
                from .context import Service
                try:
                    status = Service(str(values["server"]),
                                     str(values.get("token", ""))).reachable()
                    add("server", True, f"{values['server']} (HTTP {status})")
                except BridgeError as e:
                    add("server", False, str(e), f"check {path}")
            if values.get("token") and path.is_file() \
                    and path.stat().st_mode & 0o077:
                add("token-file", False, f"{path} is readable by others",
                    f"chmod 600 {path}")
    try:
        host = _host(args)
        inst = host.instance()
        state = host.call("state.get")
        add("idiff", True, f"pid {inst.pid}, "
            f"{len(state.get('entries', []))} entries")
    except Exception as e:
        add("idiff", False, str(e), "start idiff, or set IDIFF_BIN")
    return {"checks": checks, "ok": all(c["ok"] for c in checks)}


def cmd_install_url_handler(args: argparse.Namespace) -> Any:
    from . import urlhandler
    app = urlhandler.install_macos_handler(
        Path(args.app_dir) if args.app_dir else None)
    return {"app": str(app), "scheme": urlhandler.SCHEME}


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="idiff-bridge",
        description="Adapters between idiff and an external service.")
    sub = p.add_subparsers(dest="command", required=True)

    s = sub.add_parser("adapters", help="list installed adapters")
    s.set_defaults(func=cmd_adapters)

    s = sub.add_parser("run", help="run an adapter action")
    s.add_argument("adapter")
    s.add_argument("action")
    s.add_argument("--param", action="append", metavar="KEY=VALUE",
                   help="action argument; repeat for several")
    s.set_defaults(func=cmd_run)

    s = sub.add_parser("url", help="run the action an idiff-bridge:// link names")
    s.add_argument("link")
    s.set_defaults(func=cmd_url)

    s = sub.add_parser("current", help="the group idiff shows")
    s.set_defaults(func=cmd_current)

    s = sub.add_parser("advance", help="move between groups")
    s.add_argument("--step", type=int, default=1)
    s.set_defaults(func=cmd_advance)

    s = sub.add_parser("new", help="create an adapter from the template")
    s.add_argument("name")
    s.set_defaults(func=cmd_new)

    s = sub.add_parser("doctor", help="check an adapter's setup")
    s.add_argument("name")
    s.set_defaults(func=cmd_doctor)

    s = sub.add_parser("install-url-handler",
                       help="create the macOS app that receives links")
    s.add_argument("--app-dir")
    s.set_defaults(func=cmd_install_url_handler)
    return p


def main(argv: Optional[List[str]] = None,
         host: Optional[Host] = None) -> int:
    """Runs one command.  `host` lets tests drive a fake idiff."""
    args = build_parser().parse_args(argv)
    args.host = host
    try:
        _emit(args.func(args))
        return 0
    except BridgeError as e:
        sys.stderr.write(f"idiff-bridge: {e}\n")
        return 1
    except KeyboardInterrupt:
        return 130
