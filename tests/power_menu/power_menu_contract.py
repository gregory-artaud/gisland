#!/usr/bin/env python3
"""Exercise the shipped Lua module through its JSONL process boundary."""
import json
import os
from pathlib import Path
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import unittest

HOST, MODULE = sys.argv[1:3]
del sys.argv[1:3]


def buttons(node):
    if node.get("type") == "button":
        return [node]
    return [button for child in node.get("children", []) for button in buttons(child)]


class PowerMenuContract(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.log = Path(self.directory.name) / "commands"
        self.control_log = Path(self.directory.name) / "control"
        host = Path(self.directory.name) / "gisland-lua-host"
        shutil.copy2(HOST, host)
        control = Path(self.directory.name) / "gislandctl"
        control.write_text("#!" + sys.executable + "\nimport sys\nfrom pathlib import Path\n" +
                           "Path(" + repr(str(self.control_log)) +
                           ").open('a').write(' '.join(sys.argv[1:]) + '\\n')\n")
        control.chmod(0o755)
        self.start()
        self.addCleanup(self.close)

    def start(self, config=None):
        self.process = subprocess.Popen(
            [str(Path(self.directory.name) / "gisland-lua-host"), MODULE], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, encoding="utf-8", env=os.environ.copy())
        self.records = queue.Queue()
        threading.Thread(target=self.read, args=(self.process.stdout, self.records), daemon=True).start()
        # All command arrays are harmless, including tests that reach confirmation.
        config = config if config is not None else {name + "_command": [sys.executable, "-c",
                  "from pathlib import Path; p=Path(" + repr(str(self.log)) +
                  "); p.open('a').write(" + repr(name + "\n") + ")"]
                  for name in ("lock", "restart", "shutdown")}
        self.send({"type": "init", "instance_id": "power-menu", "locale": "fr_FR.UTF-8",
                   "timezone": "UTC", "configuration": config,
                   "protocol": {"minimum": {"major": 1, "minor": 11},
                                "maximum": {"major": 1, "minor": 11}},
                   "capabilities": ["independent-views", "escape-action"]})
        self.ready = self.next()
        self.assertEqual(self.ready["type"], "ready")
        self.initial_logs = []
        self.menu = self.next()
        while self.menu["type"] == "log":
            self.initial_logs.append(self.menu)
            self.menu = self.next()

    def close(self):
        if self.process.poll() is None:
            self.send({"type": "shutdown", "reason": "test", "deadline_ms": 1000})
            self.process.wait(timeout=3)
        for stream in (self.process.stdin, self.process.stdout, self.process.stderr):
            stream.close()

    @staticmethod
    def read(stream, records):
        for line in stream:
            records.put(json.loads(line))

    def send(self, record):
        self.process.stdin.write(json.dumps(record) + "\n")
        self.process.stdin.flush()

    def next(self):
        try:
            return self.records.get(timeout=3)
        except queue.Empty:
            self.fail(f"no module response; exit={self.process.poll()}; " +
                      (self.process.stderr.read() if self.process.poll() is not None else ""))

    def action(self, action):
        self.send({"type": "action", "action_id": action, "invocation_id": "1"})
        records = []
        while True:
            record = self.next()
            if record["type"] == "action_result":
                return record, records
            records.append(record)

    def wait_commands(self, expected):
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            if self.log.exists() and self.log.read_text().splitlines() == expected:
                return
            time.sleep(0.01)
        self.fail(f"expected commands {expected}; got " +
                  (self.log.read_text() if self.log.exists() else "none"))

    def test_lock_is_immediate_and_confirm_runs_only_the_selected_command_once(self):
        result, _ = self.action("lock")
        self.assertTrue(result["accepted"])
        self.wait_commands(["lock"])
        for action, commands in (("restart", ["lock", "restart"]),
                                 ("shutdown", ["lock", "restart", "shutdown"])):
            self.action(action)
            result, _ = self.action("confirm")
            self.assertTrue(result["accepted"])
            self.wait_commands(commands)
            result, _ = self.action("confirm")
            self.assertFalse(result["accepted"])
        self.wait_commands(["lock", "restart", "shutdown"])

    def test_hidden_resets_confirmation_before_reopen(self):
        self.send({"type": "visibility", "visibility": "expanded-active"})
        self.action("shutdown")
        self.send({"type": "visibility", "visibility": "hidden"})
        menu = self.next()
        self.assertEqual(len(buttons(menu["views"]["expanded"])), 3)
        result, _ = self.action("confirm")
        self.assertFalse(result["accepted"])
        self.assertFalse(self.log.exists())

    def test_invalid_command_arrays_are_logged_and_never_executed(self):
        self.close()
        self.start({"lock_command": [], "restart_command": ["reboot", 1],
                    "shutdown_command": [""]})
        self.assertEqual(len(self.initial_logs), 3)
        for action in ("lock", "restart", "shutdown"):
            if action != "lock":
                self.action(action)
                action = "confirm"
            result, _ = self.action(action)
            self.assertFalse(result["accepted"])
        self.assertIsNone(self.process.poll())
        self.assertFalse(self.log.exists())

    def test_failed_commands_log_and_leave_the_module_alive(self):
        self.close()
        self.start({"lock_command": ["/definitely/missing-gisland-test-command"],
                    "restart_command": [sys.executable, "-c", "raise SystemExit(7)"]})
        for action in ("lock", "restart"):
            if action == "restart":
                self.action(action)
                action = "confirm"
            result, records = self.action(action)
            self.assertTrue(result["accepted"])
            record = self.next()
            self.assertEqual(record["type"], "log")
            self.assertEqual(record["level"], "error")
        self.assertIsNone(self.process.poll())

    def test_toggle_opens_then_closes_from_confirmation(self):
        result, _ = self.action("toggle")
        self.assertTrue(result["accepted"])
        self.wait_control(["activate-open power-menu"])
        self.send({"type": "visibility", "visibility": "expanded-active"})
        self.action("restart")
        result, _ = self.action("toggle")
        self.assertTrue(result["accepted"])
        self.wait_control(["activate-open power-menu", "close"])

    def wait_control(self, expected):
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            if self.control_log.exists() and self.control_log.read_text().splitlines() == expected:
                return
            time.sleep(0.01)
        self.fail("unexpected control commands")

    def test_destructive_actions_require_confirmation_and_cancel_is_first(self):
        for action in ("restart", "shutdown"):
            result, records = self.action(action)
            self.assertTrue(result["accepted"])
            confirm = next(r for r in records if r["type"] == "publish")
            self.assertEqual(confirm["presentation"]["escape_action_id"], "cancel")
            targets = buttons(confirm["views"]["expanded"])
            self.assertEqual([b["action_id"] for b in targets], ["cancel", "confirm"])
            self.assertTrue(targets[1]["close_on_activate"])
            self.assertFalse(self.log.exists())
            result, records = self.action("cancel")
            self.assertTrue(result["accepted"])
            self.assertEqual(len(buttons(records[0]["views"]["expanded"])), 3)
        result, _ = self.action("confirm")
        self.assertFalse(result["accepted"])
        self.assertFalse(self.log.exists())

    def test_activation_only_menu_has_three_labeled_buttons(self):
        self.assertEqual(self.ready["protocol_minor"], 11)
        self.assertEqual(self.menu["type"], "publish")
        self.assertTrue(self.menu["activation_only"])
        self.assertEqual(self.menu["context_id"], "power-menu")
        row = self.menu["views"]["expanded"]
        self.assertEqual(row["type"], "row")
        targets = buttons(row)
        self.assertEqual([b["accessible_label"] for b in targets],
                         ["Verrouiller", "Redémarrer", "Éteindre"])
        self.assertEqual([b["close_on_activate"] for b in targets], [True, False, False])
        self.assertFalse(self.log.exists())


if __name__ == "__main__":
    unittest.main()
