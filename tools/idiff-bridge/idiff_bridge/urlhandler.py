"""Turns idiff-bridge:// links into CLI calls, and installs the macOS app
that receives them.

A link looks like

    idiff-bridge://<adapter>/<action>?<query>

For example idiff-bridge://aigc/open?scene=CS1&group=CASE-1601.  Only
actions marked @action(url=True) may be reached this way: any web page
can open such a link.
"""

from __future__ import annotations

import shlex
import subprocess
import sys
from pathlib import Path
from typing import List, Optional, Tuple
from urllib.parse import parse_qs, urlparse

from .context import bridge_root, cli_command
from .errors import BridgeError
from .loader import available, load_adapter_by_name, read_manifest

SCHEME = "idiff-bridge"


def parse_link(link: str) -> Tuple[str, str, dict]:
    """(adapter, action, query) from a link; `action` is "open" without one."""
    parsed = urlparse(link)
    if parsed.scheme != SCHEME:
        raise BridgeError(f"not a {SCHEME}:// link: {link}")
    host = parsed.netloc or parsed.path.split("/")[0]
    parts = [p for p in parsed.path.split("/") if p]
    adapter = host
    action = parts[0] if parts else "open"
    if not adapter:
        raise BridgeError(f"the link names no adapter: {link}")
    return adapter, action, parse_qs(parsed.query)


def open_link(link: str, host: Optional[Host] = None) -> dict:
    """Runs the action a link names, if that action allows links.

    `host` is for tests that drive a fake idiff; real callers omit it.
    """
    from .session import Host

    adapter_name, action_name, query = parse_link(link)
    adapter = load_adapter_by_name(adapter_name)
    manifest = read_manifest(available()[adapter_name])
    specs = adapter.actions()
    if action_name not in specs:
        raise BridgeError(
            f"adapter {adapter_name!r} has no action {action_name!r}")
    spec = specs[action_name]
    if not spec.url:
        raise BridgeError(
            f"action {adapter_name}/{action_name} cannot be triggered by a "
            "link")
    import inspect
    from .context import Context
    from .settings import load_settings

    sig = inspect.signature(spec.func)
    kwargs = {}
    for name, param in sig.parameters.items():
        if name == "self" or name in ("ctx", "context"):
            continue
        if name in query and query[name]:
            kwargs[name] = query[name][0]
    schema = manifest.settings or adapter.settings_schema()
    settings = load_settings(adapter_name, schema)
    ctx = Context(adapter=adapter_name, host=host or Host(),
                  settings=settings, query=query, schema=schema,
                  buttons=list(manifest.buttons))
    result = spec.func(adapter, ctx, **kwargs)
    return {"ok": True, "adapter": adapter_name, "action": action_name,
            "result": result}


APP_NAME = "idiff-bridge-link.app"


def install_macos_handler(app_dir: Optional[Path] = None,
                          scheme: str = SCHEME) -> Path:
    """Builds a .app that hands <scheme>:// links to this CLI.

    macOS delivers the whole URL as the AppleScript `open location`
    argument, so the script passes it on unchanged.  Re-run this after
    moving the idiff-bridge checkout: the command path is baked in.
    """
    if sys.platform != "darwin":
        raise BridgeError("installing a URL handler is macOS-only")
    app_dir = app_dir or (Path.home() / "Applications" / APP_NAME)
    script = (
        "on open location theURL\n"
        f"  do shell script {shlex.quote(' '.join(cli_command()))} "
        "& \" url \" & quoted form of theURL\n"
        "end open location\n"
    )
    script_path = app_dir.with_suffix(".applescript")
    script_path.parent.mkdir(parents=True, exist_ok=True)
    script_path.write_text(script, encoding="utf-8")
    if app_dir.exists():
        subprocess.run(["rm", "-rf", str(app_dir)], check=True)
    subprocess.run(["osacompile", "-o", str(app_dir), str(script_path)],
                   check=True)
    plist = app_dir / "Contents" / "Info.plist"
    _add_url_type(plist, scheme)
    return app_dir


def _add_url_type(plist: Path, scheme: str) -> None:
    """Declares the URL scheme; plutil keeps the plist in its binary form."""
    import plistlib

    with plist.open("rb") as f:
        doc = plistlib.load(f)
    doc.setdefault("CFBundleURLTypes", [])
    entry = {"CFBundleURLName": f"{scheme} links",
             "CFBundleURLSchemes": [scheme]}
    if entry not in doc["CFBundleURLTypes"]:
        doc["CFBundleURLTypes"].append(entry)
    with plist.open("wb") as f:
        plistlib.dump(doc, f)
