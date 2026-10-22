"""Framework tests: config writing, links, loading, settings, CLI.

Run with "python3 -m unittest discover -s tests" from tools/idiff-bridge.
They need neither idiff nor a network.
"""

from __future__ import annotations

import io
import json
import os
import shutil
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from idiff_bridge import (Adapter, BridgeError, Group, Item,  # noqa: E402
                          action, cli, loader, urlhandler)
from idiff_bridge.adapter import _BoundAction  # noqa: E402
from idiff_bridge.config import find_group, group_meta, write_config  # noqa: E402
from idiff_bridge.session import Host, Session, render_path  # noqa: E402
from idiff_bridge.testing import FakeIdiff, FakeService  # noqa: E402


class Sandbox:
    """Isolates XDG_CONFIG_HOME, XDG_CACHE_HOME and IDIFF_* for one test."""

    def __enter__(self) -> "Sandbox":
        self.dir = Path(tempfile.mkdtemp(prefix="idiff_bridge_fw_"))
        self.saved = {k: os.environ.get(k) for k in
                      ("XDG_CONFIG_HOME", "XDG_CACHE_HOME", "IDIFF_PID",
                       "IDIFF_BRIDGE_ADAPTERS")}
        os.environ["XDG_CONFIG_HOME"] = str(self.dir / "config")
        os.environ["XDG_CACHE_HOME"] = str(self.dir / "cache")
        os.environ.pop("IDIFF_PID", None)
        os.environ.pop("IDIFF_BRIDGE_ADAPTERS", None)
        return self

    def __exit__(self, *exc: object) -> None:
        for k, v in self.saved.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
        shutil.rmtree(self.dir, ignore_errors=True)

    def install_template(self, name: str = "demo") -> Path:
        target = Path(os.environ["XDG_CONFIG_HOME"]) / "idiff-bridge" / "adapters" / name
        shutil.copytree(ROOT / "template", target)
        for f in target.iterdir():
            if f.is_file():
                f.write_text(f.read_text(encoding="utf-8").replace("__NAME__", name),
                             encoding="utf-8")
        return target


def run_cli(*argv: str, host: "Host | None" = None) -> tuple:
    """(exit code, parsed stdout, stderr text)."""
    out, err = io.StringIO(), io.StringIO()
    with redirect_stdout(out), redirect_stderr(err):
        code = cli.main(list(argv), host=host)
    text = out.getvalue().strip()
    return code, (json.loads(text) if text else None), err.getvalue()


class ConfigTest(unittest.TestCase):
    def test_write_config_shape(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "c.json"
            write_config(path, [
                Group("g1", [Item("/a.png", "A"), Item("/b.png")],
                      description="first", meta={"case": 1}),
                Group("g2", [Item("/c.png")]),
            ])
            doc = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual([g["name"] for g in doc["groups"]], ["g1", "g2"])
            self.assertEqual(doc["groups"][0]["description"], "first")
            self.assertEqual(doc["groups"][0]["items"][0],
                             {"url": "/a.png", "title": "A"})
            self.assertNotIn("meta", doc["groups"][0])

    def test_duplicate_group_names_are_made_unique(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "c.json"
            groups = [Group("g", [Item("/a")]), Group("g", [Item("/b")]),
                      Group("g", [Item("/c")])]
            write_config(path, groups)
            doc = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual([g["name"] for g in doc["groups"]],
                             ["g", "g (2)", "g (3)"])

    def test_empty_config_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaises(BridgeError):
                write_config(Path(d) / "c.json", [])

    def test_group_meta_skips_groups_without_meta(self) -> None:
        groups = [Group("a", meta={"x": 1}), Group("b"), Group("c", meta={"y": 2})]
        self.assertEqual(group_meta(groups), {"a": {"x": 1}, "c": {"y": 2}})

    def test_find_group_by_name_and_position(self) -> None:
        groups = [Group("a"), Group("b")]
        self.assertEqual(find_group(groups, "b", 0), 1)
        self.assertEqual(find_group(groups, None, 1), 1)
        with self.assertRaises(BridgeError):
            find_group(groups, "zzz", 0)
        with self.assertRaises(BridgeError):
            find_group(groups, None, 5)


class SessionTest(unittest.TestCase):
    def test_round_trip_keeps_order_and_meta(self) -> None:
        with Sandbox():
            s = Session(pid=os.getpid(), config_path="/tmp/c.json",
                        groups={"a": {"case": "A"}}, order=["a", "b"])
            s.save()
            back = Session.load(os.getpid())
            assert back is not None
            self.assertEqual(back.order, ["a", "b"])
            self.assertEqual(back.groups, {"a": {"case": "A"}})
            Session.remove(os.getpid())
            self.assertIsNone(Session.load(os.getpid()))

    def test_a_record_for_another_pid_is_ignored(self) -> None:
        with Sandbox():
            Session(pid=4242, config_path="", groups={}).save()
            self.assertIsNone(Session.load(4243))

    def test_render_path_substitutions(self) -> None:
        base = Path("/data/session/config.json")
        self.assertEqual(render_path("{config_dir}/rel.png", base),
                         "/data/session/rel.png")
        self.assertEqual(render_path("{config}", base),
                         "/data/session/config.json")

    def test_absolute_paths_pass_through(self) -> None:
        self.assertEqual(render_path("/mnt/share/a.png", Path("/tmp/c.json")),
                         "/mnt/share/a.png")


class LinkTest(unittest.TestCase):
    def test_parse_link(self) -> None:
        adapter, act, query = urlhandler.parse_link(
            "idiff-bridge://svc/open?batch=b1&group=CASE-1")
        self.assertEqual((adapter, act), ("svc", "open"))
        self.assertEqual(query, {"batch": ["b1"], "group": ["CASE-1"]})

    def test_a_link_without_an_action_opens(self) -> None:
        self.assertEqual(urlhandler.parse_link("idiff-bridge://svc")[1], "open")

    def test_other_schemes_are_rejected(self) -> None:
        with self.assertRaises(BridgeError):
            urlhandler.parse_link("https://example.com/open")


class SampleAdapter(Adapter):
    name = "sample"

    @action(url=True)
    def show(self, ctx, name: str = "") -> str:
        return name

    @action
    def write(self, ctx, verdict: str = "x") -> str:
        return verdict


class TypedAdapter(Adapter):
    name = "typed"

    @action
    def typed(self, ctx, n: int = 0, ratio: float = 0.0, flag: bool = False,
              text: str = "") -> None:
        return None


class AdapterClassTest(unittest.TestCase):
    def test_actions_are_descriptors_carrying_their_flags(self) -> None:
        specs = SampleAdapter.actions()
        self.assertEqual(sorted(specs), ["show", "write"])
        self.assertTrue(specs["show"].url)
        self.assertFalse(specs["write"].url)

    def test_bound_action_calls_the_method(self) -> None:
        bound = SampleAdapter().show
        self.assertIsInstance(bound, _BoundAction)
        self.assertEqual(bound(object(), name="n"), "n")

    def test_a_method_without_self_is_rejected(self) -> None:
        with self.assertRaises(TypeError):
            @action
            def lonely(ctx):
                return None


class AppleScriptTest(unittest.TestCase):
    def test_handler_script_quotes_the_command_and_the_link(self) -> None:
        script = urlhandler.build_applescript(
            ["/usr/bin/python3", "/Users/a b/idiff-bridge"])
        self.assertIn("on open location theURL", script)
        self.assertIn(
            'do shell script "/usr/bin/python3 \'/Users/a b/idiff-bridge\' url "'
            " & quoted form of theURL", script)

    def test_double_quotes_and_backslashes_are_escaped(self) -> None:
        literal = urlhandler._applescript_string('a"b\\c')
        self.assertEqual(literal, '"a\\"b\\\\c"')


class LoaderTest(unittest.TestCase):
    def test_template_adapter_loads_and_declares_its_actions(self) -> None:
        with Sandbox() as box:
            directory = box.install_template("demo")
            ad = loader.load_adapter(directory)
            self.assertEqual(sorted(ad.actions()), ["mark", "open"])
            manifest = loader.read_manifest(directory)
            self.assertEqual(manifest.name, "demo")
            self.assertEqual([b["action"] for b in manifest.buttons], ["mark"])

    def test_unknown_adapter_lists_the_known_ones(self) -> None:
        with Sandbox():
            with self.assertRaises(BridgeError) as cm:
                loader.load_adapter_by_name("nope")
            self.assertIn("none", str(cm.exception))

    def test_bind_params_converts_by_annotation(self) -> None:
        manifest = loader.Manifest(name="sample")
        spec = TypedAdapter.actions()["typed"]
        self.assertEqual(
            loader.bind_params(spec, manifest,
                               {"n": "7", "ratio": "0.5", "flag": "yes",
                                "text": "t"}),
            {"n": 7, "ratio": 0.5, "flag": True, "text": "t"})

    def test_bind_params_prefers_the_manifest_type(self) -> None:
        manifest = loader.Manifest(
            name="sample", params={"typed": {"text": {"type": "int"}}})
        spec = TypedAdapter.actions()["typed"]
        self.assertEqual(loader.bind_params(spec, manifest, {"text": "3"}),
                         {"text": 3})

    def test_bind_params_rejects_unknown_and_malformed_values(self) -> None:
        manifest = loader.Manifest(name="sample")
        spec = TypedAdapter.actions()["typed"]
        with self.assertRaises(BridgeError) as cm:
            loader.bind_params(spec, manifest, {"typo": "1"})
        self.assertIn("accepted:", str(cm.exception))
        with self.assertRaises(BridgeError):
            loader.bind_params(spec, manifest, {"n": "seven"})

    def test_the_template_open_action_accepts_its_own_parameters(self) -> None:
        with Sandbox() as box:
            directory = box.install_template("demo")
            manifest = loader.read_manifest(directory)
            ad = loader.load_adapter(directory)
            self.assertEqual(
                loader.bind_params(ad.actions()["open"], manifest,
                                   {"group": "g", "batch": "b"}),
                {"group": "g", "batch": "b"})


class SettingsTest(unittest.TestCase):
    def test_environment_overrides_the_file(self) -> None:
        from idiff_bridge.settings import load_settings
        with Sandbox():
            path = Path(os.environ["XDG_CONFIG_HOME"]) / "idiff-bridge" / "demo.toml"
            path.parent.mkdir(parents=True)
            path.write_text('server = "http://file"\n', encoding="utf-8")
            os.environ["IDIFF_BRIDGE_DEMO_SERVER"] = "http://env"
            try:
                values = load_settings("demo", {"server": {"required": True}})
            finally:
                del os.environ["IDIFF_BRIDGE_DEMO_SERVER"]
            self.assertEqual(values["server"], "http://env")

    def test_a_missing_required_setting_is_reported(self) -> None:
        from idiff_bridge.settings import load_settings
        with Sandbox():
            with self.assertRaises(BridgeError) as cm:
                load_settings("demo", {"server": {"required": True}})
            self.assertIn("server", str(cm.exception))


class CliTest(unittest.TestCase):
    def test_adapters_lists_installed_adapters(self) -> None:
        with Sandbox() as box:
            box.install_template("demo")
            code, doc, _ = run_cli("adapters")
            self.assertEqual(code, 0)
            assert doc is not None
            self.assertEqual([a["name"] for a in doc["adapters"]], ["demo"])
            self.assertEqual(doc["adapters"][0]["actions"], ["mark", "open"])

    def test_new_copies_the_template_and_its_test_passes(self) -> None:
        with Sandbox():
            code, doc, err = run_cli("new", "fresh")
            self.assertEqual(code, 0, err)
            assert doc is not None
            created = Path(doc["created"])
            self.assertTrue((created / "adapter.toml").is_file())
            text = (created / "adapter.toml").read_text(encoding="utf-8")
            self.assertIn('name = "fresh"', text)
            self.assertNotIn("__NAME__", text)

            import subprocess
            proc = subprocess.run(
                [sys.executable, "-m", "unittest", "discover", "-s", str(created)],
                capture_output=True, text=True,
                env={**os.environ, "PYTHONPATH": str(ROOT)})
            self.assertEqual(proc.returncode, 0, proc.stderr[-2000:])

    def test_doctor_reports_each_check(self) -> None:
        with Sandbox() as box:
            for k in list(os.environ):
                if k.startswith("IDIFF_BRIDGE_"):
                    del os.environ[k]
            box.install_template("demo")
            # Bound to a socket that does not exist, so a developer's own
            # idiff window cannot change the outcome.
            nowhere = Host(socket_path=str(box.dir / "no-such.sock"))
            code, doc, _ = run_cli("doctor", "demo", host=nowhere)
            self.assertEqual(code, 0)
            assert doc is not None
            names = [c["check"] for c in doc["checks"]]
            self.assertEqual(names, ["adapter", "actions", "settings", "idiff"])
            settings_check = doc["checks"][2]
            self.assertFalse(settings_check["ok"])
            self.assertIn("server", settings_check["fix"])
            self.assertIn("demo.toml", settings_check["fix"])
            self.assertFalse(doc["checks"][3]["ok"])   # no idiff in the sandbox

    def test_unknown_command_fails_with_a_message_on_stderr(self) -> None:
        with Sandbox():
            code, doc, err = run_cli("run", "nope", "open")
            self.assertEqual(code, 1)
            self.assertIsNone(doc)
            self.assertIn("no adapter named", err)

    def test_url_runs_an_allowed_action_and_registers_buttons(self) -> None:
        with Sandbox() as box:
            box.install_template("demo")
            config_path = Path(os.environ["XDG_CONFIG_HOME"]) / "idiff-bridge" / "demo.toml"
            config_path.write_text('server = "http://service"\n', encoding="utf-8")
            doc = {"groups": [
                {"name": "CASE-1", "meta": {"case_id": "CASE-1"},
                 "items": [{"path": "/data/1k.png"}]},
                {"name": "CASE-2", "meta": {"case_id": "CASE-2"},
                 "items": [{"path": "/data/2k.png"}]},
            ]}
            with FakeIdiff(auto_load=True) as idiff, \
                    FakeService({"/api/config": doc}) as svc:
                config_path.write_text(f'server = "{svc.base_url}"\n',
                                       encoding="utf-8")
                # The CLI normally discovers instances through
                # /tmp/idiff-<pid>.sock; hand it a Host bound to the fake.
                host = Host(preferred_pid=idiff.pid,
                            socket_path=idiff.socket_path)
                out = urlhandler.open_link(
                    "idiff-bridge://demo/open?batch=b1&group=CASE-2", host=host)
                self.assertEqual(out["result"]["name"], "CASE-2")
                self.assertEqual(idiff.methods(),
                                 ["app.identity", "comparison_config.load",
                                  "comparison_config.switch_group", "actions.set"])
                set_call = idiff.calls[3]
                self.assertEqual(len(set_call.params["actions"]), 1)
                self.assertEqual(set_call.params["actions"][0]["label"],
                                 "Mark reviewed")
                self.assertEqual(set_call.params["actions"][0]["key"], "alt+1")
                command = set_call.params["actions"][0]["command"]
                self.assertEqual(command[-5:],
                                 ["run", "demo", "mark", "--param",
                                  "verdict=reviewed"])

    def test_url_rejects_unknown_query_parameters(self) -> None:
        with Sandbox() as box:
            box.install_template("demo")
            with FakeIdiff() as idiff:
                host = Host(preferred_pid=idiff.pid,
                            socket_path=idiff.socket_path)
                with self.assertRaises(BridgeError) as cm:
                    urlhandler.open_link(
                        "idiff-bridge://demo/open?typo=1", host=host)
            self.assertIn("no parameter 'typo'", str(cm.exception))

    def test_url_refuses_an_action_not_meant_for_links(self) -> None:
        with Sandbox() as box:
            box.install_template("demo")
            with FakeIdiff() as idiff:
                host = Host(preferred_pid=idiff.pid,
                            socket_path=idiff.socket_path)
                with self.assertRaises(BridgeError) as cm:
                    urlhandler.open_link("idiff-bridge://demo/mark", host=host)
            self.assertIn("cannot be triggered by a link", str(cm.exception))


class ButtonCommandTest(unittest.TestCase):
    def test_button_command_pins_the_path_variables(self) -> None:
        from idiff_bridge.context import action_command
        with Sandbox() as box:
            os.environ["IDIFF_BRIDGE_ADAPTERS"] = str(box.dir / "adapters")
            try:
                command = action_command("demo", "mark")
            finally:
                del os.environ["IDIFF_BRIDGE_ADAPTERS"]
            self.assertEqual(command[0], "env")
            self.assertIn(f"IDIFF_BRIDGE_ADAPTERS={box.dir / 'adapters'}", command)
            self.assertIn(f"XDG_CONFIG_HOME={os.environ['XDG_CONFIG_HOME']}",
                          command)
            self.assertEqual(command[-3:], ["run", "demo", "mark"])

    def test_button_command_never_carries_a_secret(self) -> None:
        from idiff_bridge.context import action_command
        with Sandbox():
            os.environ["IDIFF_BRIDGE_DEMO_TOKEN"] = "hunter2"
            try:
                command = action_command("demo", "mark")
            finally:
                del os.environ["IDIFF_BRIDGE_DEMO_TOKEN"]
            self.assertFalse(any("hunter2" in word for word in command))


class ServiceTest(unittest.TestCase):
    def test_reachable_accepts_any_http_answer(self) -> None:
        from idiff_bridge.context import Service
        with FakeService({}) as svc:      # "/" is a 404 here
            self.assertEqual(Service(svc.base_url).reachable(), 404)

    def test_reachable_reports_a_refused_connection(self) -> None:
        from idiff_bridge.context import Service
        with self.assertRaises(BridgeError):
            Service("http://127.0.0.1:9", timeout=2).reachable()


class HostTest(unittest.TestCase):
    def test_current_group_without_a_session_names_the_problem(self) -> None:
        with Sandbox(), FakeIdiff() as idiff:
            idiff.state["current_comparison_key"] = "config:CASE-1"
            host = Host(preferred_pid=idiff.pid, socket_path=idiff.socket_path)
            with self.assertRaises(Exception) as cm:
                host.current_group()
            self.assertIn("no bridge session recorded", str(cm.exception))

    def test_current_group_without_a_config(self) -> None:
        with Sandbox(), FakeIdiff() as idiff:
            host = Host(preferred_pid=idiff.pid, socket_path=idiff.socket_path)
            with self.assertRaises(Exception) as cm:
                host.current_group()
            self.assertIn("not showing a comparison config", str(cm.exception))

    def test_advance_stops_at_the_last_group(self) -> None:
        with Sandbox(), FakeIdiff(auto_load=True) as idiff:
            host = Host(preferred_pid=idiff.pid, socket_path=idiff.socket_path)
            host.show_config([Group("g1", [Item("/a")], meta={"n": 1}),
                              Group("g2", [Item("/b")], meta={"n": 2})],
                             Path(os.environ["XDG_CACHE_HOME"]) / "c.json")
            self.assertEqual(host.current_group()["name"], "g1")
            moved = host.advance(1)
            assert moved is not None
            self.assertEqual(moved["name"], "g2")
            self.assertIsNone(host.advance(1))
            self.assertIsNone(host.advance(-5))


if __name__ == "__main__":
    unittest.main()
