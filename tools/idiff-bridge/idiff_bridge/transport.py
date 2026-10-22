"""Minimal JSON-RPC client for idiff.

The wire format is a 4-byte big-endian length prefix followed by UTF-8
JSON, over a Unix domain socket at /tmp/idiff-<pid>.sock (a named pipe on
Windows).  Only the standard library is used.
"""

from __future__ import annotations

import json
import os
import socket
import struct
import time
from dataclasses import dataclass
from typing import Any, List, Optional

MAX_FRAME_BYTES = 64 * 1024 * 1024
SOCKET_GLOB = "/tmp/idiff-*.sock"


class RpcError(Exception):
    """The idiff server answered with a JSON-RPC error."""

    def __init__(self, code: int, message: str) -> None:
        super().__init__(message)
        self.code = code
        self.message = message


@dataclass
class Instance:
    pid: int
    socket_path: str

    def __str__(self) -> str:
        return f"idiff:{self.pid} ({self.socket_path})"


def list_sockets() -> List[str]:
    """Socket paths of every plausible idiff process, oldest name first."""
    try:
        names = [n for n in os.listdir("/tmp")
                 if n.startswith("idiff-") and n.endswith(".sock")]
    except OSError:
        return []
    paths = ["/tmp/" + n for n in names]
    return sorted(paths, key=_numeric_pid)


def _numeric_pid(path: str) -> int:
    stem = os.path.basename(path)[len("idiff-"):-len(".sock")]
    try:
        return int(stem)
    except ValueError:
        return 0


class Client:
    """One connection.  Reuse it for several calls, then close()."""

    def __init__(self, socket_path: str, timeout: float = 10.0) -> None:
        self.socket_path = socket_path
        self.timeout = timeout
        self._sock: Optional[socket.socket] = None
        self._next_id = 1

    def __enter__(self) -> "Client":
        return self

    def __exit__(self, *exc: Any) -> None:
        self.close()

    def connect(self) -> None:
        if self._sock is not None:
            return
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(self.timeout)
        s.connect(self.socket_path)
        self._sock = s

    def close(self) -> None:
        if self._sock is not None:
            try:
                self._sock.close()
            finally:
                self._sock = None

    def call(self, method: str, params: Optional[dict] = None,
             _retry: bool = True) -> Any:
        """Sends one request.  A connection the peer has already closed is
        reopened once: liveness shows only on write, and a request that was
        never written cannot be duplicated."""
        try:
            return self._call_once(method, params)
        except (BrokenPipeError, ConnectionResetError):
            self.close()
            if not _retry:
                raise
            return self.call(method, params, _retry=False)

    def _call_once(self, method: str, params: Optional[dict] = None) -> Any:
        self.connect()
        assert self._sock is not None
        req_id = self._next_id
        self._next_id += 1
        body = json.dumps({"jsonrpc": "2.0", "method": method,
                           "params": params or {}, "id": req_id},
                          separators=(",", ":")).encode()
        self._sock.sendall(struct.pack(">I", len(body)) + body)
        raw = self._recv_frame()
        resp = json.loads(raw)
        if "error" in resp:
            err = resp["error"]
            raise RpcError(int(err.get("code", -32603)),
                           str(err.get("message", "")))
        return resp.get("result")

    def _recv_frame(self) -> bytes:
        assert self._sock is not None
        head = self._recv_exact(4)
        (n,) = struct.unpack(">I", head)
        if n > MAX_FRAME_BYTES:
            raise RpcError(-32603, f"frame too large ({n} bytes)")
        return self._recv_exact(n)

    def _recv_exact(self, n: int) -> bytes:
        assert self._sock is not None
        buf = bytearray()
        while len(buf) < n:
            chunk = self._sock.recv(n - len(buf))
            if not chunk:
                raise RpcError(-32603, "connection closed by idiff")
            buf.extend(chunk)
        return bytes(buf)


def probe(path: str, timeout: float = 2.0) -> Optional[Instance]:
    """Return the Instance behind `path` when it answers app.identity."""
    try:
        with Client(path, timeout=timeout) as c:
            ident = c.call("app.identity")
    except Exception:
        return None
    try:
        pid = int(ident["pid"])
    except (KeyError, TypeError, ValueError):
        return None
    return Instance(pid=pid, socket_path=path)


def instance_for_pid(pid: int) -> Optional[Instance]:
    path = SOCKET_GLOB.replace("*", str(pid))
    return probe(path)


def discover(preferred_pid: Optional[int] = None) -> Instance:
    """The instance an action should drive.

    `preferred_pid` (normally IDIFF_PID from the environment) wins when
    that process answers.  Without it, or when it has exited, the only
    live instance is used; several live instances without a preferred
    pid is an error, because guessing would drive the wrong window.
    """
    if preferred_pid is not None:
        found = instance_for_pid(preferred_pid)
        if found is not None:
            return found
    live = [i for i in (probe(p) for p in list_sockets()) if i is not None]
    if not live:
        raise RpcError(-32603, "no running idiff instance")
    if len(live) > 1:
        raise RpcError(
            -32603,
            "several idiff instances are running ("
            + ", ".join(str(i) for i in live)
            + "); run the action from the idiff window it is meant for, "
              "or set IDIFF_PID")
    return live[0]
