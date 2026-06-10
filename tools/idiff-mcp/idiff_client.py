"""idiff_client -- thin client for the idiff JSON-RPC server.

The idiff GUI ships a JSON-RPC 2.0 server bound to:
  - POSIX:   Unix Domain Socket at /tmp/idiff-<pid>.sock
  - Windows: Named Pipe at \\\\.\\pipe\\idiff-<pid>

This module provides:

  * `discover_instances()` -- list every live transport path
  * `IdiffClient`          -- a synchronous request/response client
                              (4-byte BE length prefix + UTF-8 JSON)

The MCP server uses these primitives.  They have no MCP dependency
themselves so they can also be used from ad-hoc scripts.
"""
from __future__ import annotations

import json
import os
import socket
import struct
import sys
import time
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
WIN_PIPE_PREFIX = r"\\.\pipe\idiff-"


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
    """Could not reach the server (no socket/pipe, refused, broken pipe)."""


# ---------------------------------------------------------------------
# Windows named-pipe transport via ctypes

if sys.platform == "win32":
    import ctypes
    from ctypes import wintypes

    _kernel32 = ctypes.windll.kernel32

    # 64-bit safety: default c_int return truncates HANDLE values.
    _kernel32.CreateFileW.restype = wintypes.HANDLE
    _kernel32.CreateFileW.argtypes = [
        wintypes.LPCWSTR,  # lpFileName
        wintypes.DWORD,    # dwDesiredAccess
        wintypes.DWORD,    # dwShareMode
        wintypes.LPVOID,   # lpSecurityAttributes
        wintypes.DWORD,    # dwCreationDisposition
        wintypes.DWORD,    # dwFlagsAndAttributes
        wintypes.HANDLE,   # hTemplateFile
    ]
    _kernel32.WriteFile.restype = wintypes.BOOL
    _kernel32.WriteFile.argtypes = [
        wintypes.HANDLE,   # hFile
        wintypes.LPCVOID,  # lpBuffer
        wintypes.DWORD,    # nNumberOfBytesToWrite
        ctypes.POINTER(wintypes.DWORD),  # lpNumberOfBytesWritten
        wintypes.LPVOID,   # lpOverlapped
    ]
    _kernel32.ReadFile.restype = wintypes.BOOL
    _kernel32.ReadFile.argtypes = [
        wintypes.HANDLE,   # hFile
        wintypes.LPVOID,   # lpBuffer
        wintypes.DWORD,    # nNumberOfBytesToRead
        ctypes.POINTER(wintypes.DWORD),  # lpNumberOfBytesRead
        wintypes.LPVOID,   # lpOverlapped
    ]
    _kernel32.CloseHandle.restype = wintypes.BOOL
    _kernel32.CloseHandle.argtypes = [wintypes.HANDLE]

    _GENERIC_READ = 0x80000000
    _GENERIC_WRITE = 0x40000000
    _OPEN_EXISTING = 3
    _INVALID_HANDLE_VALUE = wintypes.HANDLE(-1).value or -1

    class _WinPipeTransport:
        """Low-level byte I/O over a Windows named pipe via ctypes."""

        def __init__(self, path: str, timeout: float):
            self._path = path
            self._timeout = timeout
            self._handle: Optional[int] = None

        def connect(self) -> None:
            if self._handle is not None:
                return
            handle = _kernel32.CreateFileW(
                self._path,
                _GENERIC_READ | _GENERIC_WRITE,
                0,              # no sharing
                None,           # default security
                _OPEN_EXISTING,
                0,              # default attributes
                None,           # no template
            )
            if handle == _INVALID_HANDLE_VALUE:
                err = ctypes.get_last_error() or ctypes.GetLastError()
                raise IdiffConnectionError(
                    f"CreateFileW({self._path}) failed: error {err}")
            self._handle = handle

        def close(self) -> None:
            if self._handle is not None:
                _kernel32.CloseHandle(self._handle)
                self._handle = None

        def sendall(self, data: bytes) -> None:
            assert self._handle is not None
            written = wintypes.DWORD()
            if not _kernel32.WriteFile(
                    self._handle, data, len(data),
                    ctypes.byref(written), None):
                err = ctypes.GetLastError()
                raise IdiffConnectionError(
                    f"WriteFile failed: error {err}")
            if written.value != len(data):
                raise IdiffConnectionError(
                    f"WriteFile: wrote {written.value}/{len(data)} bytes")

        def read_exact(self, n: int) -> bytes:
            assert self._handle is not None
            buf = bytearray()
            deadline = time.monotonic() + self._timeout
            while len(buf) < n:
                remaining = n - len(buf)
                to_read = min(remaining, 65536)
                read_buf = ctypes.create_string_buffer(to_read)
                got = wintypes.DWORD()
                ok = _kernel32.ReadFile(
                    self._handle,
                    read_buf,
                    to_read,
                    ctypes.byref(got),
                    None,
                )
                if not ok or got.value == 0:
                    if len(buf) > 0:
                        raise IdiffConnectionError(
                            f"pipe closed mid-frame "
                            f"(read {len(buf)}/{n})")
                    raise IdiffConnectionError(
                        "pipe closed or ReadFile failed")
                buf.extend(read_buf.raw[:got.value])
                if time.monotonic() > deadline:
                    raise IdiffConnectionError(
                        f"timeout reading from pipe "
                        f"({len(buf)}/{n} bytes)")
            return bytes(buf)


class IdiffClient:
    """Blocking JSON-RPC client over AF_UNIX or Windows named pipe.

    Opens one connection per instance and reuses it for the lifetime of
    the object.  Not thread-safe -- create one per worker if you fan out.
    """

    def __init__(self, socket_path: str, timeout: float = DEFAULT_TIMEOUT_S):
        self.socket_path = socket_path
        self._timeout = timeout
        self._transport: Optional[
            _WinPipeTransport | socket.socket
        ] = None
        self._next_id = 1

    # ------------------------------------------------------------------
    # Lifecycle

    def connect(self) -> None:
        if self._transport is not None:
            return

        if sys.platform == "win32":
            t = _WinPipeTransport(self.socket_path, self._timeout)
            try:
                t.connect()
            except IdiffConnectionError:
                raise
            except OSError as ex:
                raise IdiffConnectionError(
                    f"connect({self.socket_path}) failed: {ex}") from ex
            self._transport = t
        else:
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
            self._transport = s

    def close(self) -> None:
        if self._transport is not None:
            try:
                self._transport.close()
            finally:
                self._transport = None

    def __enter__(self) -> "IdiffClient":
        self.connect()
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.close()

    # ------------------------------------------------------------------
    # Framing

    def _read_exact(self, n: int) -> bytes:
        if sys.platform == "win32" and isinstance(self._transport,
                                                   _WinPipeTransport):
            return self._transport.read_exact(n)
        # POSIX path
        assert isinstance(self._transport, socket.socket)
        buf = bytearray()
        while len(buf) < n:
            chunk = self._transport.recv(n - len(buf))
            if not chunk:
                raise IdiffConnectionError(
                    f"server closed mid-frame (read {len(buf)}/{n})")
            buf += chunk
        return bytes(buf)

    def _send_frame(self, body: bytes) -> None:
        if len(body) > MAX_FRAME_BYTES:
            raise IdiffRpcError(-32600, "request too large")
        data = struct.pack(">I", len(body)) + body
        if sys.platform == "win32" and isinstance(self._transport,
                                                   _WinPipeTransport):
            self._transport.sendall(data)
        else:
            assert isinstance(self._transport, socket.socket)
            self._transport.sendall(data)

    def _recv_frame(self) -> bytes:
        hdr = self._read_exact(4)
        n = struct.unpack(">I", hdr)[0]
        if n > MAX_FRAME_BYTES:
            raise IdiffConnectionError(
                f"server announced frame of {n} bytes "
                f"(cap {MAX_FRAME_BYTES})")
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


def _enumerate_pipes_win32() -> list[str]:
    """Enumerate idiff named pipes on Windows via FindFirstFileW."""
    import ctypes
    from ctypes import wintypes

    class WIN32_FIND_DATAW(ctypes.Structure):
        _fields_ = [
            ("dwFileAttributes", wintypes.DWORD),
            ("ftCreationTime", wintypes.FILETIME),
            ("ftLastAccessTime", wintypes.FILETIME),
            ("ftLastWriteTime", wintypes.FILETIME),
            ("nFileSizeHigh", wintypes.DWORD),
            ("nFileSizeLow", wintypes.DWORD),
            ("dwReserved0", wintypes.DWORD),
            ("dwReserved1", wintypes.DWORD),
            ("cFileName", ctypes.c_wchar * 260),
            ("cAlternateFileName", ctypes.c_wchar * 14),
        ]

    kernel32 = ctypes.windll.kernel32

    # Must set restype/argtypes: on 64-bit, default c_int return
    # truncates HANDLE and causes access violations.
    kernel32.FindFirstFileW.restype = wintypes.HANDLE
    kernel32.FindFirstFileW.argtypes = [
        wintypes.LPCWSTR,
        ctypes.POINTER(WIN32_FIND_DATAW),
    ]
    kernel32.FindNextFileW.restype = wintypes.BOOL
    kernel32.FindNextFileW.argtypes = [
        wintypes.HANDLE,
        ctypes.POINTER(WIN32_FIND_DATAW),
    ]
    kernel32.FindClose.restype = wintypes.BOOL
    kernel32.FindClose.argtypes = [wintypes.HANDLE]

    INVALID_HANDLE_VALUE = wintypes.HANDLE(-1).value

    find_data = WIN32_FIND_DATAW()
    hfind = kernel32.FindFirstFileW(r"\\.\pipe\idiff-*", ctypes.byref(find_data))
    if hfind == INVALID_HANDLE_VALUE:
        return []

    paths: list[str] = []
    try:
        while True:
            name = find_data.cFileName
            if name:
                paths.append(r"\\.\pipe\\" + name)
            if not kernel32.FindNextFileW(hfind, ctypes.byref(find_data)):
                break
    finally:
        kernel32.FindClose(hfind)
    return paths


def discover_instances() -> list[Instance]:
    """Return every live idiff process reachable on this machine.

    POSIX:   walks /tmp/idiff-*.sock
    Windows: enumerates ``\\\\.\\pipe\\idiff-*``
    Probes each with app.identity, drops the ones that don't answer or
    that don't identify as 'idiff'.  Sorted by pid for stable output.
    Empty list when nothing is running.
    """
    out: list[Instance] = []

    if sys.platform == "win32":
        paths = _enumerate_pipes_win32()
    else:
        import glob
        paths = sorted(glob.glob(SOCKET_GLOB))

    for path in sorted(paths):
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
