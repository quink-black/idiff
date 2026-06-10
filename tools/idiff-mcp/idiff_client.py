"""idiff_client -- thin client for the idiff JSON-RPC server.

The idiff GUI ships a JSON-RPC 2.0 server bound to a Unix Domain
Socket at /tmp/idiff-<pid>.sock.  This module provides:

  * `discover_instances()` -- list every live socket
  * `IdiffClient`          -- a synchronous request/response client
                              (4-byte BE length prefix + UTF-8 JSON)

The MCP server uses these primitives.  They have no MCP dependency
themselves so they can also be used from ad-hoc scripts.
"""
from __future__ import annotations

import errno
import glob
import json
import os
import socket
import struct
from dataclasses import dataclass
from typing import Any, Optional

# Wire framing: 4-byte big-endian length prefix + JSON body.  Hard
# server-side cap is 64 MiB; keep ours generous but bounded so a buggy
# server cannot overrun us.
MAX_FRAME_BYTES = 64 * 1024 * 1024

# Default per-call timeout, in seconds.  Most idiff RPCs are
# non-blocking from the server's point of view (they run inside one
# pump of frame()), but image decoding inside library.load can take a
# moment for HEIF / large RAW.  10 s gives generous headroom without
# letting a stuck call hang the agent indefinitely.
DEFAULT_TIMEOUT_S = 10.0

SOCKET_GLOB = "/tmp/idiff-*.sock"


@dataclass
class Instance:
    """One live idiff process discovered on this machine."""
    pid: int
    socket_path: str
    label: str          # e.g. "idiff:12345"

    def __str__(self) -> str:
        return f"{self.label} ({self.socket_path})"


class IdiffRpcError(RuntimeError):
    """The server returned a JSON-RPC error envelope."""

    def __init__(self, code: int, message: str, data: Any = None):
        super().__init__(f"JSON-RPC error {code}: {message}")
        self.code = code
        self.message = message
        self.data = data


class IdiffConnectionError(RuntimeError):
    """Could not reach the server (no socket, refused, broken pipe)."""


class IdiffClient:
    """Blocking JSON-RPC client over AF_UNIX.

    Opens one socket per instance and reuses it for the lifetime of the
    object.  Not thread-safe -- create one per worker if you fan out.
    """

    def __init__(self, socket_path: str, timeout: float = DEFAULT_TIMEOUT_S):
        self.socket_path = socket_path
        self._timeout = timeout
        self._sock: Optional[socket.socket] = None
        self._next_id = 1

    # ------------------------------------------------------------------
    # Lifecycle

    def connect(self) -> None:
        if self._sock is not None:
            return
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(self._timeout)
        try:
            s.connect(self.socket_path)
        except FileNotFoundError as ex:
            raise IdiffConnectionError(
                f"socket not found: {self.socket_path}") from ex
        except ConnectionRefusedError as ex:
            raise IdiffConnectionError(
                f"connection refused (stale socket?): "
                f"{self.socket_path}") from ex
        except OSError as ex:
            raise IdiffConnectionError(
                f"connect({self.socket_path}) failed: {ex}") from ex
        self._sock = s

    def close(self) -> None:
        if self._sock is not None:
            try:
                self._sock.close()
            finally:
                self._sock = None

    def __enter__(self) -> "IdiffClient":
        self.connect()
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.close()

    # ------------------------------------------------------------------
    # Framing

    def _read_exact(self, n: int) -> bytes:
        assert self._sock is not None
        buf = bytearray()
        while len(buf) < n:
            chunk = self._sock.recv(n - len(buf))
            if not chunk:
                raise IdiffConnectionError(
                    f"server closed mid-frame (read {len(buf)}/{n})")
            buf += chunk
        return bytes(buf)

    def _send_frame(self, body: bytes) -> None:
        assert self._sock is not None
        if len(body) > MAX_FRAME_BYTES:
            raise IdiffRpcError(-32600, "request too large")
        self._sock.sendall(struct.pack(">I", len(body)) + body)

    def _recv_frame(self) -> bytes:
        hdr = self._read_exact(4)
        n = struct.unpack(">I", hdr)[0]
        if n > MAX_FRAME_BYTES:
            raise IdiffConnectionError(
                f"server announced frame of {n} bytes (cap {MAX_FRAME_BYTES})")
        return self._read_exact(n) if n > 0 else b""

    # ------------------------------------------------------------------
    # JSON-RPC

    def call(self, method: str, params: Optional[dict] = None) -> Any:
        """Invoke `method`; return the result on success, raise on error."""
        self.connect()
        req_id = self._next_id
        self._next_id += 1
        req = {
            "jsonrpc": "2.0",
            "method": method,
            "params": params if params is not None else {},
            "id": req_id,
        }
        self._send_frame(json.dumps(req, separators=(",", ":")).encode())
        body = self._recv_frame()
        if not body:
            # Server returned an empty response (notification framing).
            # We sent an id, so this is a protocol violation; surface
            # it rather than silently returning None.
            raise IdiffConnectionError(
                "server returned empty frame to a request")
        resp = json.loads(body)
        if "error" in resp:
            err = resp["error"]
            raise IdiffRpcError(
                int(err.get("code", -32603)),
                str(err.get("message", "")),
                err.get("data"))
        return resp.get("result")


# ---------------------------------------------------------------------
# Discovery

def _probe_socket(path: str, timeout: float = 0.5) -> Optional[Instance]:
    """Attempt one app.identity round-trip.

    Returns an Instance on success.  Returns None for any kind of
    failure (path gone, ECONNREFUSED, server doesn't speak our
    protocol, response missing identity fields).  Identity probing
    deliberately swallows errors -- this is best-effort discovery,
    not a health check.
    """
    try:
        with IdiffClient(path, timeout=timeout) as c:
            ident = c.call("app.identity")
    except Exception:
        return None
    if not isinstance(ident, dict) or ident.get("name") != "idiff":
        return None
    pid = ident.get("pid")
    label = ident.get("label") or (f"idiff:{pid}" if pid else "idiff:?")
    if not isinstance(pid, int):
        return None
    return Instance(pid=pid, socket_path=path, label=label)


def discover_instances() -> list[Instance]:
    """Return every live idiff process reachable on this machine.

    Walks /tmp/idiff-*.sock, probes each with app.identity, drops the
    ones that don't answer or that don't identify as 'idiff'.  Sorted
    by pid for stable output.  Empty list when nothing is running.
    """
    out: list[Instance] = []
    for path in sorted(glob.glob(SOCKET_GLOB)):
        inst = _probe_socket(path)
        if inst is not None:
            out.append(inst)
    out.sort(key=lambda i: i.pid)
    return out


def select_instance(
    explicit_pid: Optional[int] = None,
) -> tuple[Optional[Instance], list[Instance]]:
    """Pick the instance an MCP tool should target.

    Resolution order:
      1. If `explicit_pid` is given, only that instance is acceptable.
      2. Else, if exactly one live instance exists, use it.
      3. Else (zero or many), the caller is responsible for asking the
         user; we return (None, all_live_instances).

    Returns (instance, all_live_instances).  `instance` is None when
    the caller needs to escalate to the user.
    """
    instances = discover_instances()
    if explicit_pid is not None:
        for i in instances:
            if i.pid == explicit_pid:
                return i, instances
        return None, instances
    if len(instances) == 1:
        return instances[0], instances
    return None, instances
