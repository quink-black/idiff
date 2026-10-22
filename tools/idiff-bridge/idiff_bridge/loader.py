"""Find adapters on disk, read their adapter.toml, coerce action arguments."""

from __future__ import annotations

import importlib.util
import os
import sys
from pathlib import Path
from typing import Any, Dict, Optional, Type

from .adapter import Adapter, Manifest, collect_actions
from .errors import BridgeError

try:  # Python 3.11+
    import tomllib as _toml
except ModuleNotFoundError:  # pragma: no cover - older interpreters
    _toml = None


def adapters_root() -> Path:
    """Where adapters live: $IDIFF_BRIDGE_ADAPTERS, else ~/.config."""
    env = os.environ.get("IDIFF_BRIDGE_ADAPTERS")
    if env:
        return Path(env)
    base = os.environ.get("XDG_CONFIG_HOME") or str(Path.home() / ".config")
    return Path(base) / "idiff-bridge" / "adapters"


def available() -> Dict[str, Path]:
    """name -> adapter directory."""
    root = adapters_root()
    found: Dict[str, Path] = {}
    if root.is_dir():
        for child in sorted(root.iterdir()):
            if (child / "adapter.toml").is_file():
                found[child.name] = child
    return found


def _parse_toml(path: Path) -> Dict[str, Any]:
    if _toml is None:
        raise BridgeError(
            "Python 3.11 or newer is required to read adapter.toml")
    try:
        return _toml.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        raise BridgeError(f"{path}: {e}")


def read_manifest(directory: Path) -> Manifest:
    doc = _parse_toml(directory / "adapter.toml")
    m = Manifest(
        name=str(doc.get("name", directory.name)),
        entry=str(doc.get("entry", "")),
        settings=dict(doc.get("settings", {})),
        params={k: dict(v) for k, v in doc.get("params", {}).items()},
        buttons=[dict(b) for b in doc.get("buttons", [])],
        notes=str(doc.get("notes", "")),
    )
    if not m.entry:
        raise BridgeError(f"{directory}/adapter.toml: missing 'entry'")
    if m.name != directory.name:
        raise BridgeError(
            f"{directory}/adapter.toml: name {m.name!r} does not match the "
            f"directory {directory.name!r}")
    return m


def load_adapter(directory: Path) -> Adapter:
    """Instantiate the adapter described by `directory`."""
    manifest = read_manifest(directory)
    module_name, _, attr = manifest.entry.partition(":")
    if not attr:
        raise BridgeError(
            f"{directory}/adapter.toml: entry must be 'module.py:ClassName'")
    module_path = directory / module_name
    if not module_path.is_file():
        raise BridgeError(f"{module_path}: not found")

    # The adapter imports idiff_bridge by name, so the checkout must be
    # importable even when an adapter directory is the current directory.
    root = str(Path(__file__).resolve().parent.parent)
    if root not in sys.path:
        sys.path.insert(0, root)

    spec = importlib.util.spec_from_file_location(
        f"idiff_bridge_adapters.{manifest.name}", module_path)
    if spec is None or spec.loader is None:
        raise BridgeError(f"{module_path}: cannot be imported")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)

    cls: Optional[Type[Adapter]] = getattr(module, attr, None)
    if cls is None:
        raise BridgeError(f"{module_path}: no attribute {attr!r}")
    adapter = cls()
    adapter.name = manifest.name
    actions = collect_actions(cls)
    if not actions:
        raise BridgeError(f"{module_path}: {attr} declares no @action")
    adapter._actions = actions  # noqa: SLF001 - framework-owned attribute
    return adapter


def load_adapter_by_name(name: str) -> Adapter:
    found = available()
    if name not in found:
        raise BridgeError(
            f"no adapter named {name!r}; known: "
            + (", ".join(found) or "none")
            + f" (looked in {adapters_root()})")
    return load_adapter(found[name])


_MISSING = object()


def coerce_params(adapter: Adapter, manifest: Manifest,
                  action_name: str, raw: Dict[str, str]) -> Dict[str, Any]:
    """Turn command-line strings into the types the action declares.

    Types come from `[params.<action>.<arg>]` in adapter.toml, defaulting
    to string.  Unknown arguments are an error: a typo in a key binding
    should be visible, not silently dropped.
    """
    declared = manifest.params.get(action_name, {})
    out: Dict[str, Any] = {}
    for key, text in raw.items():
        spec = declared.get(key, {"type": "string"})
        kind = str(spec.get("type", "string"))
        try:
            if kind == "int":
                out[key] = int(text)
            elif kind == "float":
                out[key] = float(text)
            elif kind == "bool":
                out[key] = text.lower() in ("1", "true", "yes", "on")
            else:
                out[key] = text
        except ValueError:
            raise BridgeError(f"--{key}: {text!r} is not a valid {kind}")
        if key not in declared:
            raise BridgeError(
                f"action {action_name!r} does not declare a parameter {key!r}")
    return out
