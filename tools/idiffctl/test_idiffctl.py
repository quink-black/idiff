"""Tests for idiffctl against fake idiff servers on Unix sockets.

Each test points discovery at a private temporary directory, so a real
idiff running on the same machine is never contacted.  POSIX only.

Run from this directory:  python3 -m unittest -v test_idiffctl
"""
from __future__ import annotations

import io
import json
import os
import shutil
import signal
import socket
import struct
import sys
import tempfile
import threading
import time
import unittest
import warnings
from contextlib import redirect_stderr, redirect_stdout
from typing import Any, Callable, Optional
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.realpath(__file__)))

import idiff_client  # noqa: E402
import idiffctl  # noqa: E402


class RpcFail(Exception):
    def __init__(self, code: int, message: str):
        super().__init__(message)
        self.code = code


def _recv_exact(conn: socket.socket, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            return b""
        buf += chunk
    return buf


class FakeIdiff:
    """Answers idiff JSON-RPC on <sock_dir>/idiff-<pid>.sock and records
    every call other than app.identity."""

    def __init__(self, sock_dir: str, pid: int,
                 entries: tuple[str, ...] = (),
                 handlers: Optional[dict[str, Callable[[Any], Any]]] = None):
        self.pid = pid
        self.path = os.path.join(sock_dir, f"idiff-{pid}.sock")
        self.entries = list(entries)
        self.calls: list[tuple[str, Any]] = []
        self.handlers: dict[str, Callable[[Any], Any]] = {
            "app.identity": lambda p: {"name": "idiff", "pid": pid,
                                       "socket": self.path,
                                       "label": f"idiff:{pid}"},
            "state.get": lambda p: self.state(),
        }
        self.handlers.update(handlers or {})
        self._sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._sock.bind(self.path)
        self._sock.listen(16)
        threading.Thread(target=self._serve, daemon=True).start()

    def state(self) -> dict:
        return {
            "entries": [{"index": i, "path": p, "width": 4, "height": 4,
                         "frames": 1} for i, p in enumerate(self.entries)],
            "selection": [0] if self.entries else [],
            "reference": 0 if self.entries else None,
            "comparison_references": {},
            "view": {"mode": "overlay", "slider": 0.5},
        }

    def methods(self) -> list[str]:
        return [m for m, _ in self.calls]

    def close(self) -> None:
        self._sock.close()
        try:
            os.unlink(self.path)
        except FileNotFoundError:
            pass

    def _serve(self) -> None:
        while True:
            try:
                conn, _ = self._sock.accept()
            except OSError:
                return
            threading.Thread(target=self._session, args=(conn,),
                             daemon=True).start()

    def _session(self, conn: socket.socket) -> None:
        with conn:
            try:
                while True:
                    hdr = _recv_exact(conn, 4)
                    if not hdr:
                        return
                    body = _recv_exact(conn, struct.unpack(">I", hdr)[0])
                    req = json.loads(body)
                    method, params = req["method"], req.get("params")
                    if method != "app.identity":
                        self.calls.append((method, params))
                    resp: dict = {"jsonrpc": "2.0", "id": req["id"]}
                    handler = self.handlers.get(method)
                    try:
                        if handler is None:
                            raise RpcFail(-32601, f"no method {method}")
                        resp["result"] = handler(params)
                    except RpcFail as ex:
                        resp["error"] = {"code": ex.code,
                                         "message": str(ex)}
                    out = json.dumps(resp).encode()
                    conn.sendall(struct.pack(">I", len(out)) + out)
            except OSError:
                return


def serve_as_child(sock_dir: str, paths: list[str]) -> None:
    """Entry point of the fake idiff executable used by the open tests."""
    FakeIdiff(sock_dir, os.getpid(), entries=tuple(paths))
    while True:
        time.sleep(60)


@unittest.skipIf(sys.platform == "win32", "Unix domain sockets only")
class IdiffctlTest(unittest.TestCase):
    def setUp(self) -> None:
        self.dir = tempfile.mkdtemp(prefix="idc")
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)
        glob = mock.patch.object(idiff_client, "SOCKET_GLOB",
                                 os.path.join(self.dir, "idiff-*.sock"))
        glob.start()
        self.addCleanup(glob.stop)
        env = mock.patch.dict(os.environ)
        env.start()
        self.addCleanup(env.stop)
        os.environ.pop("IDIFF_PID", None)
        os.environ.pop("IDIFF_BIN", None)

    def server(self, pid: int = 900001, **kw) -> FakeIdiff:
        s = FakeIdiff(self.dir, pid, **kw)
        self.addCleanup(s.close)
        return s

    def run_cli(self, *argv: str) -> tuple[int, Any, str]:
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            try:
                code = idiffctl.main(list(argv))
            except SystemExit as ex:
                code = ex.code
        text = out.getvalue()
        return code, (json.loads(text) if text else None), err.getvalue()

    def touch(self, name: str) -> str:
        path = os.path.join(self.dir, name)
        open(path, "wb").close()
        return path

    # --- instance selection -----------------------------------------

    def test_no_instance_exits_3(self):
        code, out, err = self.run_cli("state")
        self.assertEqual(code, idiffctl.EXIT_NO_INSTANCE)
        self.assertIsNone(out)
        self.assertIn("no idiff instance", err)

    def test_single_instance_is_targeted(self):
        srv = self.server(entries=("/a.png", "/b.png"))
        code, out, _ = self.run_cli("state")
        self.assertEqual(code, 0)
        self.assertEqual(len(out["entries"]), 2)
        self.assertEqual(srv.methods(), ["state.get"])

    def test_several_instances_need_a_pid(self):
        self.server(900001)
        second = self.server(900002, entries=("/x.png",))
        code, _, err = self.run_cli("state")
        self.assertEqual(code, idiffctl.EXIT_AMBIGUOUS)
        self.assertIn("900001", err)
        self.assertIn("900002", err)

        for argv in (("--pid", "900002", "state"),
                     ("state", "--pid", "900002")):
            code, out, _ = self.run_cli(*argv)
            self.assertEqual(code, 0, argv)
            self.assertEqual(out["entries"][0]["path"], "/x.png")

        os.environ["IDIFF_PID"] = "900002"
        code, out, _ = self.run_cli("state")
        self.assertEqual(code, 0)
        self.assertEqual(len(second.calls), 3)

    def test_unknown_pid_exits_4(self):
        self.server(900001)
        code, _, err = self.run_cli("--pid", "123", "state")
        self.assertEqual(code, idiffctl.EXIT_AMBIGUOUS)
        self.assertIn("123", err)

    def test_instances_lists_every_server(self):
        self.server(900001)
        self.server(900002)
        code, out, _ = self.run_cli("instances")
        self.assertEqual(code, 0)
        self.assertEqual([i["pid"] for i in out["instances"]],
                         [900001, 900002])

    # --- errors -----------------------------------------------------

    def test_rpc_error_exits_1_with_message(self):
        def reject(params):
            raise RpcFail(-32602, "index out of range: 9 (have 2 entries)")
        self.server(handlers={"library.set_reference": reject})
        code, out, err = self.run_cli("ref", "9")
        self.assertEqual(code, idiffctl.EXIT_RPC)
        self.assertIsNone(out)
        self.assertIn("index out of range", err)

    def test_timeout_exits_5(self):
        self.server(handlers={"state.get": lambda p: time.sleep(1.0) or {}})
        code, _, err = self.run_cli("--timeout", "0.2", "state")
        self.assertEqual(code, idiffctl.EXIT_CONNECTION)
        self.assertIn("state.get", err)

    def test_usage_error_exits_2(self):
        self.server()
        self.assertEqual(self.run_cli("view")[0], idiffctl.EXIT_USAGE)
        self.assertEqual(self.run_cli("call", "x", "not json")[0],
                         idiffctl.EXIT_USAGE)
        self.assertEqual(self.run_cli("select", "1", "--clear")[0],
                         idiffctl.EXIT_USAGE)
        self.assertEqual(self.run_cli("bogus")[0], idiffctl.EXIT_USAGE)

    # --- request shapes ---------------------------------------------

    def test_entry_tokens_map_to_index_or_path(self):
        srv = self.server(handlers={
            "library.set_reference": lambda p: {},
            "library.remove": lambda p: {},
            "metrics.compare": lambda p: {"results": []},
            "pixel.sample": lambda p: {"samples": []},
            "selection.set": lambda p: {},
        })
        self.run_cli("ref", "3")
        self.run_cli("rm", "/a b.png")
        self.run_cli("metrics", "--ref", "0", "/b.png", "2")
        self.run_cli("sample", "10", "20", "/c.png")
        self.run_cli("select", "0", "/d.png")
        self.run_cli("ref", "rel/e.png")
        self.assertEqual(srv.calls, [
            ("library.set_reference", {"index": 3}),
            ("library.remove", {"path": "/a b.png"}),
            ("metrics.compare", {"ref": 0, "targets": ["/b.png", 2]}),
            ("pixel.sample", {"x": 10, "y": 20, "entries": ["/c.png"]}),
            ("selection.set", {"entries": [0, "/d.png"]}),
            ("library.set_reference",
             {"path": os.path.abspath("rel/e.png")}),
        ])

    def test_metrics_without_arguments_uses_server_defaults(self):
        srv = self.server(handlers={"metrics.compare": lambda p: {"ok": 1}})
        code, out, _ = self.run_cli("metrics")
        self.assertEqual((code, out), (0, {"ok": 1}))
        self.assertEqual(srv.calls, [("metrics.compare", {})])

    def test_view_applies_changes_in_order(self):
        names = ("view.set_group_mode", "view.set_mode", "view.set_channel",
                 "view.set_zoom_pan")
        srv = self.server(handlers={n: (lambda p: {}) for n in names})
        code, out, _ = self.run_cli(
            "view", "--zoom", "2", "--channel", "y", "--mode", "difference",
            "--group-mode", "by_folder", "--pan-x", "5")
        self.assertEqual(code, 0)
        self.assertEqual(out["applied"], list(names))
        self.assertEqual(srv.calls, [
            ("view.set_group_mode", {"mode": "by_folder"}),
            ("view.set_mode", {"mode": "difference"}),
            ("view.set_channel", {"channel": "y"}),
            ("view.set_zoom_pan", {"zoom": 2.0, "pan_x": 5.0}),
        ])

    def test_view_slider_alone_keeps_the_current_mode(self):
        srv = self.server(handlers={"view.set_mode": lambda p: {}})
        self.run_cli("view", "--slider", "0.25")
        self.assertEqual(srv.calls[-1],
                         ("view.set_mode", {"mode": "overlay",
                                            "slider": 0.25}))

    def test_call_passes_method_and_params_through(self):
        srv = self.server(handlers={"timeline.set_frame_offset":
                                    lambda p: {"echo": p}})
        code, out, _ = self.run_cli("call", "timeline.set_frame_offset",
                                    '{"path": "/a.png", "offset": -2}')
        self.assertEqual(code, 0)
        self.assertEqual(out, {"echo": {"path": "/a.png", "offset": -2}})
        self.assertEqual(srv.methods(), ["timeline.set_frame_offset"])

    def test_paths_are_made_absolute(self):
        srv = self.server(handlers={"library.load": lambda p: {"added": 1},
                                    "view.screenshot": lambda p: p})
        self.touch("rel.png")
        old = os.getcwd()
        os.chdir(self.dir)
        self.addCleanup(os.chdir, old)
        self.run_cli("load", "rel.png")
        self.run_cli("shot", "out.png")
        self.assertEqual(srv.calls[0][1]["paths"],
                         [os.path.join(os.getcwd(), "rel.png")])
        self.assertEqual(srv.calls[1][1]["path"],
                         os.path.join(os.getcwd(), "out.png"))
        self.assertEqual(self.run_cli("load", "missing.png")[0],
                         idiffctl.EXIT_USAGE)

    def test_state_brief_keeps_only_selected_entries(self):
        self.server(entries=("/a.png", "/b.png", "/c.png"))
        code, out, _ = self.run_cli("state", "--brief")
        self.assertEqual(code, 0)
        self.assertNotIn("entries", out)
        self.assertEqual(out["entry_count"], 3)
        self.assertEqual([e["path"] for e in out["selected"]], ["/a.png"])

    # --- open -------------------------------------------------------

    def fake_binary(self) -> str:
        path = os.path.join(self.dir, "fake-idiff")
        here = os.path.dirname(os.path.realpath(__file__))
        with open(path, "w") as f:
            f.write(f"#!{sys.executable}\n"
                    "import sys\n"
                    f"sys.path.insert(0, {here!r})\n"
                    "import test_idiffctl\n"
                    f"test_idiffctl.serve_as_child({self.dir!r}, "
                    "sys.argv[1:])\n")
        os.chmod(path, 0o755)
        return path

    def test_open_loads_into_the_running_instance(self):
        srv = self.server(handlers={"library.load": lambda p: {"added": 1}})
        a = self.touch("a.png")
        code, out, _ = self.run_cli("open", a, "--bin", "/nonexistent")
        self.assertEqual(code, 0)
        self.assertFalse(out["launched"])
        self.assertEqual(out["pid"], srv.pid)
        self.assertEqual(srv.calls[0], ("library.load", {"paths": [a]}))

    def test_open_starts_idiff_when_none_is_running(self):
        a, b = self.touch("a.png"), self.touch("b.png")
        with warnings.catch_warnings():
            # The launched process outlives its Popen object by design.
            warnings.simplefilter("ignore", ResourceWarning)
            code, out, err = self.run_cli("open", a, b, "--wait", "10",
                                          "--bin", self.fake_binary())
        self.assertEqual(code, 0, err)
        self.addCleanup(self.reap, out["pid"])
        self.assertTrue(out["launched"])
        self.assertEqual(out["state"]["entry_count"], 2)
        self.assertEqual(out["socket"],
                         os.path.join(self.dir, f"idiff-{out['pid']}.sock"))

    def test_open_with_bad_binary_exits_2(self):
        a = self.touch("a.png")
        code, _, err = self.run_cli("open", a, "--bin", "/nonexistent")
        self.assertEqual(code, idiffctl.EXIT_USAGE)
        self.assertIn("/nonexistent", err)

    @staticmethod
    def reap(pid: int) -> None:
        try:
            os.kill(pid, signal.SIGTERM)
            os.waitpid(pid, 0)
        except (ProcessLookupError, ChildProcessError):
            pass


if __name__ == "__main__":
    unittest.main()
