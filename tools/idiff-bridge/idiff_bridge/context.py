"""What an action may reach: idiff, its service, and the user's settings.

Adapters get a Context and nothing else, so the surface they can depend
on is the whole framework API.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional

from .config import Group, find_group
from .errors import BridgeError
from .session import DEFAULT_CONFIG_PATH, Host, render_path


def bridge_root() -> Path:
    """Directory of the idiff-bridge checkout the CLI runs from."""
    return Path(__file__).resolve().parent.parent


def cli_command() -> List[str]:
    """argv prefix that invokes this CLI."""
    return [sys.executable, str(bridge_root() / "idiff-bridge")]


# Variables that decide where the CLI finds adapters, settings and the
# session record.  A button runs in idiff's environment, not in the shell
# that registered it, so these are fixed into the button's command.
# Settings variables (IDIFF_BRIDGE_<NAME>_<KEY>) are left out: they carry
# secrets, and the command line is visible to other users and in
# actions.get.
_PINNED_ENV = ("XDG_CONFIG_HOME", "XDG_CACHE_HOME", "IDIFF_BRIDGE_ADAPTERS",
               "IDIFF_BIN")


def action_command(adapter: str, action: str) -> List[str]:
    """argv a panel button runs for `adapter`'s `action`."""
    pinned = [f"{k}={os.environ[k]}" for k in _PINNED_ENV if os.environ.get(k)]
    prefix = ["env"] + pinned if pinned else []
    return prefix + cli_command() + ["run", adapter, action]


class Service:
    """The external HTTP service an adapter talks to.

    `base_url` and `token` come from the user's settings; requests carry
    the token as a bearer credential.  Failures are raised as BridgeError
    with the status code and the server's message, because that is what
    the person clicking the button needs to see.
    """

    def __init__(self, base_url: str, token: str = "",
                 timeout: float = 15.0) -> None:
        self.base_url = base_url.rstrip("/")
        self.token = token
        self.timeout = timeout

    def _request(self, method: str, path: str, body: Any = None,
                 query: Optional[Dict[str, Any]] = None) -> Any:
        url = self.base_url + path
        if query:
            url += "?" + urllib.parse.urlencode(query, doseq=True)
        data = None
        headers = {"Accept": "application/json"}
        if body is not None:
            data = json.dumps(body).encode()
            headers["Content-Type"] = "application/json"
        if self.token:
            headers["Authorization"] = "Bearer " + self.token
        req = urllib.request.Request(url, data=data, headers=headers,
                                     method=method)
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                raw = resp.read()
        except urllib.error.HTTPError as e:
            detail = ""
            try:
                detail = e.read().decode("utf-8", "replace")[:400]
            except Exception:
                pass
            raise BridgeError(f"{method} {path}: HTTP {e.code} {detail}".strip())
        except urllib.error.URLError as e:
            raise BridgeError(f"{method} {url}: {e.reason}")
        if not raw:
            return None
        try:
            return json.loads(raw)
        except ValueError:
            raise BridgeError(f"{method} {path}: response is not JSON")

    def reachable(self) -> int:
        """HTTP status of the base URL; raises BridgeError when unreachable.

        Any HTTP answer, including 401 or 404, proves the service is up;
        most services have no handler for "/".
        """
        req = urllib.request.Request(self.base_url + "/", method="GET")
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                return int(resp.status)
        except urllib.error.HTTPError as e:
            e.close()
            return int(e.code)
        except urllib.error.URLError as e:
            raise BridgeError(f"{self.base_url}: {e.reason}")

    def get_json(self, path: str, query: Optional[Dict[str, Any]] = None) -> Any:
        return self._request("GET", path, query=query)

    def post_json(self, path: str, body: Any) -> Any:
        return self._request("POST", path, body=body)


def notify(title: str, message: str) -> None:
    """Best-effort desktop notification; silence is acceptable."""
    try:
        if sys.platform == "darwin":
            script = (f'display notification {json.dumps(message)} '
                      f'with title {json.dumps(title)}')
            subprocess.run(["osascript", "-e", script], check=False,
                           stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=5)
        elif shutil.which("notify-send"):
            subprocess.run(["notify-send", title, message], check=False,
                           stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=5)
    except Exception:
        pass


@dataclass
class Context:
    """Handed to every action.

    `query` is the part of a idiff-bridge:// link after the action name,
    unparsed; `settings` holds the values from the user's settings file
    for this adapter, with required entries already checked.
    """

    adapter: str
    host: Host
    settings: Dict[str, Any]
    query: Dict[str, List[str]]
    config_path: Path = Path(DEFAULT_CONFIG_PATH)
    # Settings schema from adapter.toml; load_settings() needs it.
    schema: Dict[str, Dict[str, Any]] = field(default_factory=dict)
    # [[buttons]] from adapter.toml; registered after show_config().
    buttons: List[Dict[str, Any]] = field(default_factory=list)

    # -- config -------------------------------------------------------
    def show_config(self, groups: List[Group], group: Optional[str] = None,
                    index: int = 0) -> Dict[str, Any]:
        """Load `groups` into idiff, open one of them, and register the
        manifest's buttons in the actions panel.

        The buttons survive until another config replaces them or idiff
        exits.  When idiff does not support actions, the config still
        loads and `buttons_error` reports why no buttons appeared.
        """
        result = self.host.show_config(groups, self.config_path, group=group,
                                       index=index)
        if self.buttons:
            try:
                self.register_buttons()
            except Exception as e:  # older idiff without actions.set
                result["buttons_error"] = str(e)
        return result

    def register_buttons(self) -> int:
        """Replaces the session buttons with the manifest's [[buttons]]."""
        actions = []
        for b in self.buttons:
            action = str(b.get("action", ""))
            if not action:
                raise BridgeError("adapter.toml: [[buttons]] needs 'action'")
            command = action_command(self.adapter, action)
            for k, v in (b.get("args") or {}).items():
                command += ["--param", f"{k}={v}"]
            entry: Dict[str, Any] = {"label": str(b.get("label") or action),
                                     "command": command}
            if b.get("key"):
                entry["key"] = str(b["key"])
            actions.append(entry)
        return int(self.idiff.call("actions.set", {"actions": actions})["count"])

    def current_group(self) -> Dict[str, Any]:
        """The group idiff shows: {pid, name, meta, config_path}."""
        return self.host.current_group()

    def advance(self, step: int = 1) -> Optional[Dict[str, Any]]:
        """Move `step` groups; None when the last group is already shown."""
        return self.host.advance(step)

    # -- buttons ------------------------------------------------------
    def act(self, labels: Dict[str, str],
            params: Optional[Dict[str, Dict[str, Any]]] = None,
            replace: bool = True) -> int:
        """Puts buttons in idiff's actions panel.

        `labels` maps an action name of this adapter to the button caption;
        `params` gives that action's arguments, e.g.
        {"score": {"verdict": "better", "key": "alt+1"}}.  The "key"
        entry, when present, becomes the button's shortcut.  Returns the
        total number of buttons including the user's own.
        """
        actions = []
        for name, label in labels.items():
            spec = params.get(name, {}) if params else {}
            key = spec.get("key")
            command = action_command(self.adapter, name)
            for k, v in spec.items():
                if k == "key":
                    continue
                command += ["--param", f"{k}={v}"]
            entry: Dict[str, Any] = {"label": label, "command": command}
            if key:
                entry["key"] = key
            actions.append(entry)
        if not replace:
            existing = self.idiff.call("actions.get").get("actions", [])
            actions = [a for a in existing if a.get("origin") == "session"] + actions
        return int(self.idiff.call("actions.set", {"actions": actions})["count"])

    # -- idiff --------------------------------------------------------
    @property
    def idiff(self) -> Host:
        """The idiff window; use it for RPC methods the Context lacks."""
        return self.host

    # -- external -----------------------------------------------------
    @property
    def http(self) -> Service:
        base = self.settings.get("server")
        if not base:
            raise BridgeError(
                f"setting 'server' is not configured; set it in {self.adapter}.toml")
        return Service(str(base), str(self.settings.get("token", "")))

    def notify(self, message: str, title: Optional[str] = None) -> None:
        notify(title or self.adapter, message)

    # -- paths -------------------------------------------------------
    def config_file(self, template: str) -> Path:
        """Resolves a config path that may use {config_dir} and {config}."""
        return Path(render_path(template, self.config_path))


__all__ = ["Context", "Service", "notify", "find_group", "cli_command",
           "action_command"]
