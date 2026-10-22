"""Tests for the __NAME__ adapter, written against the framework's fakes.

They need neither idiff nor the service: FakeIdiff speaks the JSON-RPC
wire format idiff does, and FakeService answers canned JSON.  Run them
with "python3 -m unittest discover -s <this directory>".

The imports below assume the idiff-bridge checkout is on PYTHONPATH; the
CLI sets that up for real runs.
"""

from __future__ import annotations

import unittest

from idiff_bridge import Group, Item
from idiff_bridge.testing import FakeIdiff, FakeService, fake_context

import adapter as module


CONFIG = {
    "groups": [
        {"name": "CASE-1", "description": "first",
         "meta": {"case_id": "CASE-1"},
         "items": [{"path": "/data/1k/CASE-1.png", "title": "1K"},
                   {"path": "/data/2k/CASE-1.png", "title": "2K"}]},
        {"name": "CASE-2", "meta": {"case_id": "CASE-2"},
         "items": [{"path": "/data/1k/CASE-2.png"}]},
    ]
}


class AdapterTest(unittest.TestCase):
    def setUp(self) -> None:
        self.adapter = module.Adapter()

    def test_open_loads_the_config_and_switches_to_the_named_group(self):
        with FakeIdiff(auto_load=True) as idiff, \
                FakeService({"/api/config": CONFIG}) as svc:
            ctx = fake_context("__NAME__", idiff,
                               {"server": svc.base_url})
            result = self.adapter.open(ctx, group="CASE-2", batch="b1")

            self.assertEqual(svc.gets, [("/api/config", {"batch": ["b1"]})])
            self.assertEqual(idiff.methods(),
                             ["app.identity", "comparison_config.load",
                              "comparison_config.switch_group"])
            self.assertEqual(idiff.calls[2].params, {"group_index": 1})
            self.assertEqual(result["name"], "CASE-2")

    def test_mark_posts_the_verdict_of_the_current_group(self):
        with FakeIdiff() as idiff, FakeService({}, {"/api/verdict": 204}) as svc:
            idiff.state["config_groups"] = [g["name"] for g in CONFIG["groups"]]
            idiff.state["current_comparison_key"] = "config:CASE-1"
            ctx = fake_context("__NAME__", idiff, {"server": svc.base_url})
            # The session record is written by show_config; reproduce it.
            from idiff_bridge.session import Session
            Session(pid=idiff.pid, config_path="",
                    groups={"CASE-1": {"case_id": "CASE-1"},
                            "CASE-2": {"case_id": "CASE-2"}},
                    order=["CASE-1", "CASE-2"]).save()

            self.adapter.mark(ctx, verdict="better")

            self.assertEqual(svc.posts,
                             [("/api/verdict",
                               {"case": "CASE-1", "verdict": "better"})])
            self.assertIn("comparison_config.switch_group", idiff.methods())

    def test_mark_without_a_session_record_fails_loudly(self):
        with FakeIdiff() as idiff:
            idiff.state["current_comparison_key"] = "config:CASE-1"
            ctx = fake_context("__NAME__", idiff, {"server": "http://unused"})
            from idiff_bridge.session import Session
            Session.remove(idiff.pid)
            with self.assertRaises(Exception):
                self.adapter.mark(ctx, verdict="better")

    def test_malformed_service_response_is_reported(self):
        with FakeIdiff() as idiff, FakeService({"/api/config": {"nope": 1}}) as svc:
            ctx = fake_context("__NAME__", idiff, {"server": svc.base_url})
            with self.assertRaises(Exception):
                self.adapter.open(ctx, batch="b1")


if __name__ == "__main__":
    unittest.main()
