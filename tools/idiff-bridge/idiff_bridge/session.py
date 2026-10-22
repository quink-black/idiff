"""Per-idiff record of the config a bridge loaded, and group navigation.

idiff does not remember who wrote the config it shows.  The session
record fills that gap: it maps the active group's name to the metadata
the adapter attached, so an action can answer "which object on the
server is the user looking at".
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional

from .config import Group, group_meta, write_config
from .errors import BridgeError, ResourceNotFound
from .transport import Client, Instance, discover, instance_for_pid, probe

DEFAULT_CONFIG_PATH = "/tmp/idiff-bridge-config.json"


def cache_dir() -> Path:
    base = os.environ.get("XDG_CACHE_HOME") or str(Path.home() / ".cache")
    return Path(base) / "idiff-bridge"


def render_path(template: str, config_path: Path) -> str:
    """Substitute {config_dir} and {config} in a config path template."""
    return (template.replace("{config_dir}", str(config_path.parent))
                    .replace("{config}", str(config_path)))


def _find_idiff_binary() -> Optional[str]:
    env = os.environ.get("IDIFF_BIN")
    if env:
        return env
    found = shutil.which("idiff")
    if found:
        return found
    candidates = [
        "/Applications/idiff.app/Contents/MacOS/idiff",
        str(Path.home() / "Applications/idiff.app/Contents/MacOS/idiff"),
    ]
    for c in candidates:
        if os.access(c, os.X_OK):
            return c
    return None


def launch_idiff() -> None:
    """Start idiff so the config can be loaded into it."""
    binary = _find_idiff_binary()
    if not binary:
        raise BridgeError(
            "idiff is not running and no executable was found; set IDIFF_BIN")
    subprocess.Popen([binary], stdout=subprocess.DEVNULL,
                     stderr=subprocess.DEVNULL, start_new_session=True)
    deadline = time.monotonic() + 20.0
    while time.monotonic() < deadline:
        for path in sorted(Path("/tmp").glob("idiff-*.sock")):
            try:
                inst = instance_for_pid(int(path.stem.split("-")[1]))
            except (IndexError, ValueError):
                continue
            if inst is not None:
                return
        time.sleep(0.2)
    raise BridgeError("idiff did not start listening within 20 s")


@dataclass
class Session:
    pid: int
    config_path: str
    groups: Dict[str, Dict[str, Any]] = field(default_factory=dict)
    # Group names in config order; navigation needs the order, which a
    # JSON object does not preserve.
    order: List[str] = field(default_factory=list)

    @staticmethod
    def path_for(pid: int) -> Path:
        return cache_dir() / f"session-{pid}.json"

    def save(self) -> None:
        p = self.path_for(self.pid)
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(json.dumps(
            {"pid": self.pid, "config_path": self.config_path,
             "groups": self.groups, "order": self.order},
            ensure_ascii=False, indent=1),
            encoding="utf-8")

    @staticmethod
    def load(pid: int) -> Optional["Session"]:
        p = Session.path_for(pid)
        if not p.is_file():
            return None
        try:
            doc = json.loads(p.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return None
        if int(doc.get("pid", -1)) != pid:
            return None
        return Session(pid=pid, config_path=doc.get("config_path", ""),
                       groups=doc.get("groups", {}),
                       order=list(doc.get("order", [])))

    @staticmethod
    def remove(pid: int) -> None:
        try:
            Session.path_for(pid).unlink()
        except OSError:
            pass



class Host:
    """Everything an action may do to idiff: load a config, query the
    active group, move between groups."""

    def __init__(self, preferred_pid: Optional[int] = None,
                 socket_path: Optional[str] = None) -> None:
        pid = preferred_pid
        if pid is None:
            env = os.environ.get("IDIFF_PID")
            pid = int(env) if env and env.isdigit() else None
        self.preferred_pid = pid
        # Set by tests that run a fake server on a temporary socket.
        self.socket_path = socket_path
        self._instance: Optional[Instance] = None

    # -- instance -----------------------------------------------------
    def instance(self, start: bool = False) -> Instance:
        if self._instance is None:
            if self.socket_path:
                found = probe(self.socket_path)
                self._instance = found or Instance(
                    pid=self.preferred_pid or os.getpid(),
                    socket_path=self.socket_path, label="idiff:?")
                return self._instance
            try:
                self._instance = discover(self.preferred_pid)
            except Exception:
                if not start:
                    raise
                launch_idiff()
                self._instance = discover(self.preferred_pid)
        return self._instance

    def call(self, method: str, params: Optional[dict] = None) -> Any:
        with Client(self.instance().socket_path) as c:
            return c.call(method, params)

    # -- config -------------------------------------------------------
    def show_config(self, groups: List[Group], config_path: Path,
                    group: Optional[str] = None, index: int = 0) -> Dict[str, Any]:
        """Write `groups` to `config_path`, load it in idiff, open a group."""
        write_config(config_path, groups)
        inst = self.instance(start=True)
        result = self.call("comparison_config.load",
                           {"path": str(config_path)})
        groups_now = result.get("groups", 0)
        if groups_now != len(groups):
            raise BridgeError(
                f"idiff loaded {groups_now} of {len(groups)} groups; see its "
                "status bar for the parse error")
        target = 0
        if group is not None or index:
            from .config import find_group
            target = find_group(groups, group, index)
            self.call("comparison_config.switch_group", {"group_index": target})
        session = Session(pid=inst.pid, config_path=str(config_path),
                          groups=group_meta(groups),
                          order=[g.name for g in groups])
        session.save()
        return {"pid": inst.pid, "groups": len(groups), "group": target,
                "name": groups[target].name if groups else ""}

    # -- navigation ---------------------------------------------------
    def current_group(self) -> Dict[str, Any]:
        """The group idiff shows: {pid, name, meta, config_path}."""
        inst = self.instance()
        with Client(inst.socket_path) as c:
            state = c.call("state.get")
        key = state.get("current_comparison_key")
        if not key or not key.startswith("config:"):
            raise ResourceNotFound(
                "idiff is not showing a comparison config loaded by a bridge")
        name = key[len("config:"):]
        session = Session.load(inst.pid)
        if session is None:
            raise ResourceNotFound(
                f"no bridge session recorded for idiff:{inst.pid}; load the "
                "config through an adapter first")
        meta = session.groups.get(name)
        if meta is None:
            # A group the adapter gave no meta to is unknown here rather
            # than wrong: report the name and let the adapter decide.
            names = session.order or list(session.groups)
            raise ResourceNotFound(
                f"group {name!r} of idiff:{inst.pid} carries no meta; "
                f"groups with meta: {', '.join(sorted(session.groups)) or 'none'}"
                f" (config has {len(names)} groups)")
        return {"pid": inst.pid, "name": name, "meta": meta,
                "config_path": session.config_path}

    def advance(self, step: int) -> Optional[Dict[str, Any]]:
        """Move `step` groups; the new group, or None at the last one."""
        inst = self.instance()
        with Client(inst.socket_path) as c:
            state = c.call("state.get")
            key = state.get("current_comparison_key") or ""
            if not key.startswith("config:"):
                raise ResourceNotFound("idiff is not showing a comparison config")
            name = key[len("config:"):]
            session = Session.load(inst.pid)
            names = _ordered_names(session, name)
            idx = names.index(name) if name in names else -1
            target = idx + step
            if idx < 0 or target < 0 or target >= len(names):
                return None
            c.call("comparison_config.switch_group", {"group_index": target})
        return self.current_group()


def _ordered_names(session: Optional[Session], current: str) -> List[str]:
    """Group names in config order.

    Records written before `order` existed fall back to the config file
    the bridge wrote, since idiff's state does not list the groups.
    """
    if session is not None and session.order:
        return session.order
    if session is not None and session.config_path:
        try:
            doc = json.loads(Path(session.config_path).read_text(
                encoding="utf-8"))
            return [g["name"] for g in doc.get("groups", [])]
        except (OSError, ValueError, KeyError, TypeError):
            pass
    if session is not None and current in session.groups:
        return [current]
    return [current]
