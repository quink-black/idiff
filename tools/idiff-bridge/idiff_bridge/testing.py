"""Fakes that let an adapter's tests run without idiff, macOS or a service.

Typical test:

    from idiff_bridge.testing import FakeIdiff, FakeService, run_action

    with FakeIdiff() as idiff, FakeService({"/api/idiff-config": doc}) as svc:
        out = run_action(MyAdapter().open, ctx=..., query={"group": ["g2"]})
        assert svc.posts == []
        assert idiff.calls[0][0] == "comparison_config.load"

The fakes speak the same wire format idiff does and record every call, so
a test asserts on the calls rather than on the GUI.
"""

from __future__ import annotations

import json
import os
import socket
import struct
import tempfile
import threading
from dataclasses import dataclass, field
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional, Tuple


@dataclass
class RecordedCall:
    method: str
    params: Dict[str, Any]


class FakeIdiff:
    """A minimal idiff RPC server on a temporary Unix socket."""

    def __init__(self, auto_load: bool = False,
                 state: Optional[Dict[str, Any]] = None) -> None:
        """"auto_load" makes comparison_config.load fill "config_groups"
        from the config file, as idiff does.  Tests of actions that do not
        call show_config should leave it off: then load fails and the
        bridge does not wait for an idiff that will never appear."""
        self.auto_load = auto_load
        self.calls: List[RecordedCall] = []
        self.state = state or {
            "entries": [], "selection": [],
            "current_comparison_key": None,
            "config_groups": [],
        }
        self.socket_path = ""
        self.pid = os.getpid()
        self._sock: Optional[socket.socket] = None
        self._thread: Optional[threading.Thread] = None
        self._stop = threading.Event()

    def __enter__(self) -> "FakeIdiff":
        tmp = Path(tempfile.mkdtemp(prefix="idiff_bridge_test_"))
        self.socket_path = str(tmp / "idiff.sock")
        self._sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._sock.bind(self.socket_path)
        self._sock.listen(8)
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()
        return self

    def __exit__(self, *exc: Any) -> None:
        self._stop.set()
        if self._sock is not None:
            # shutdown() wakes the thread blocked in accept().
            try:
                self._sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self._sock.close()
        if self._thread is not None:
            self._thread.join(timeout=2)
        try:
            os.unlink(self.socket_path)
        except OSError:
            pass

    def methods(self) -> List[str]:
        return [c.method for c in self.calls]

    def _serve(self) -> None:
        assert self._sock is not None
        while not self._stop.is_set():
            try:
                conn, _ = self._sock.accept()
            except OSError:
                return
            with conn:
                self._handle(conn)

    def _handle(self, conn: socket.socket) -> None:
        try:
            head = _recv_exact(conn, 4)
            (n,) = struct.unpack(">I", head)
            req = json.loads(_recv_exact(conn, n))
        except (OSError, ValueError):
            return
        method = str(req.get("method", ""))
        params = req.get("params") or {}
        self.calls.append(RecordedCall(method, params))
        try:
            result = self._dispatch(method, params)
            reply: Dict[str, Any] = {"jsonrpc": "2.0", "id": req.get("id"),
                                     "result": result}
        except _RpcFailure as e:
            reply = {"jsonrpc": "2.0", "id": req.get("id"),
                     "error": {"code": e.code, "message": e.message}}
        body = json.dumps(reply).encode()
        conn.sendall(struct.pack(">I", len(body)) + body)

    def _dispatch(self, method: str, params: Dict[str, Any]) -> Any:
        if method == "app.identity":
            return {"pid": self.pid, "name": "idiff", "socket": self.socket_path,
                    "label": f"idiff:{self.pid}"}
        if method == "state.get":
            return dict(self.state)
        if method == "comparison_config.load":
            if self.auto_load:
                try:
                    doc = json.loads(Path(params["path"]).read_text(
                        encoding="utf-8"))
                    self.state["config_groups"] = [g["name"]
                                                   for g in doc["groups"]]
                except (OSError, ValueError, KeyError, TypeError) as e:
                    raise _RpcFailure(-32603, f"cannot load config: {e}")
            groups = self.state.get("config_groups") or []
            self.state["current_comparison_key"] = (
                "config:" + groups[0] if groups else None)
            return {"entries": 1, "groups": len(groups), "current_group": 0}
        if method == "comparison_config.switch_group":
            groups = self.state.get("config_groups") or []
            idx = int(params.get("group_index", 0))
            if idx < 0 or idx >= len(groups):
                raise _RpcFailure(-32602, "group_index out of range")
            self.state["current_comparison_key"] = "config:" + groups[idx]
            return {"entries": 1, "current_group": idx}
        if method == "actions.set":
            return {"count": len(params.get("actions", []))}
        if method == "actions.get":
            return {"actions": []}
        raise _RpcFailure(-32601, f"unknown method {method}")


class _RpcFailure(Exception):
    def __init__(self, code: int, message: str) -> None:
        super().__init__(message)
        self.code = code
        self.message = message


def _recv_exact(conn: socket.socket, n: int) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            raise OSError("closed")
        buf.extend(chunk)
    return bytes(buf)


class FakeService:
    """An HTTP server answering canned JSON and recording requests."""

    def __init__(self, routes: Dict[str, Any],
                 posts: Optional[Dict[str, Any]] = None) -> None:
        self.routes = routes
        self.post_status = posts or {}
        self.posts: List[Tuple[str, Any]] = []
        self.gets: List[Tuple[str, Dict[str, List[str]]]] = []
        self._httpd: Optional[ThreadingHTTPServer] = None
        self._thread: Optional[threading.Thread] = None

    @property
    def base_url(self) -> str:
        assert self._httpd is not None
        host, port = self._httpd.server_address[:2]
        return f"http://{host}:{port}"

    def __enter__(self) -> "FakeService":
        outer = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *a: Any) -> None:
                pass

            def do_GET(self) -> None:  # noqa: N802 - http.server API
                from urllib.parse import parse_qs, urlparse
                parsed = urlparse(self.path)
                outer.gets.append((parsed.path, parse_qs(parsed.query)))
                if parsed.path not in outer.routes:
                    self.send_error(404)
                    return
                body = json.dumps(outer.routes[parsed.path]).encode()
                self.close_connection = True
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_POST(self) -> None:  # noqa: N802 - http.server API
                n = int(self.headers.get("Content-Length", "0"))
                raw = self.rfile.read(n) if n else b""
                body = json.loads(raw) if raw else None
                outer.posts.append((self.path, body))
                status = outer.post_status.get(self.path, 200)
                self.close_connection = True
                self.send_response(status)
                self.send_header("Content-Length", "0")
                self.end_headers()

        self._httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self._thread = threading.Thread(target=self._httpd.serve_forever,
                                        daemon=True)
        self._thread.start()
        return self

    def __exit__(self, *exc: Any) -> None:
        if self._httpd is not None:
            self._httpd.shutdown()
            self._httpd.server_close()


def fake_context(adapter: str, idiff: FakeIdiff, settings: Dict[str, Any],
                 query: Optional[Dict[str, List[str]]] = None,
                 config_path: Optional[Path] = None) -> Any:
    """A Context wired to the fakes, for tests that call actions directly."""
    from .context import Context
    from .session import Host

    host = Host(preferred_pid=idiff.pid, socket_path=idiff.socket_path)
    path = config_path or (Path(tempfile.mkdtemp(prefix="idiff_bridge_cfg_"))
                           / "config.json")
    return Context(adapter=adapter, host=host, settings=settings,
                   query=query or {}, config_path=path)
