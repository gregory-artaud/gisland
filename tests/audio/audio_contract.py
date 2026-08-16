#!/usr/bin/env python3
import argparse
from collections import deque
import json
import os
import queue
import shutil
import subprocess
import tempfile
import threading
import time
import unittest
from pathlib import Path


CAPABILITIES = [
    "independent-views",
    "compact-view-styles",
    "icon-roles",
    "progress-transitions",
]


class ModuleProcess:
    def __init__(self, command, environment, init):
        self._records = queue.Queue()
        self._pending_records = deque()
        self._stderr_lines = queue.Queue()
        self._next_invocation = 1
        self.process = subprocess.Popen(
            command,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            env=environment,
        )
        threading.Thread(target=self._read_stdout, daemon=True).start()
        threading.Thread(target=self._read_stderr, daemon=True).start()
        self.process.stdin.write(json.dumps(init, separators=(",", ":")) + "\n")
        self.process.stdin.flush()
        ready = self.next_record()
        if ready.get("type") != "ready":
            raise AssertionError(f"expected ready, got {ready!r}")
        self.ready = ready

    def _read_stdout(self):
        for line in self.process.stdout:
            self._records.put(json.loads(line))

    def _read_stderr(self):
        for line in self.process.stderr:
            self._stderr_lines.put(line.rstrip("\n"))

    def next_record(self, timeout=3.0):
        if self._pending_records:
            return self._pending_records.popleft()
        return self._next_raw_record(timeout)

    def _next_raw_record(self, timeout=3.0):
        try:
            return self._records.get(timeout=timeout)
        except queue.Empty as error:
            stderr = []
            while True:
                try:
                    stderr.append(self._stderr_lines.get_nowait())
                except queue.Empty:
                    break
            raise AssertionError(
                f"timed out waiting for module JSONL; exit={self.process.poll()}; stderr={stderr!r}"
            ) from error

    def action(self, action_id: str):
        invocation_id = str(self._next_invocation)
        self._next_invocation += 1
        self.process.stdin.write(
            json.dumps(
                {
                    "type": "action",
                    "action_id": action_id,
                    "invocation_id": invocation_id,
                },
                separators=(",", ":"),
            )
            + "\n"
        )
        self.process.stdin.flush()
        action_logs = []
        records = []
        while True:
            record = self._next_raw_record(timeout=4.0)
            if (
                record.get("type") == "action_result"
                and record.get("invocation_id") == invocation_id
            ):
                if record.get("action_id") != action_id:
                    raise AssertionError(
                        f"correlated action_result has wrong action_id: {record!r}"
                    )
                result = dict(record)
                result["log"] = "\n".join(action_logs)
                result["records"] = [
                    *records,
                    {
                        "type": "action_result",
                        "action_id": action_id,
                        "accepted": record["accepted"],
                    },
                ]
                return result
            if record.get("type") == "log":
                action_logs.append(record.get("message", ""))
            else:
                records.append(record)

    def assert_no_record(self, timeout=0.15):
        try:
            record = self._next_raw_record(timeout)
        except AssertionError:
            return
        raise AssertionError(f"unexpected module record after action result: {record!r}")

    def wait_for_stderr(self, text: str, timeout=3.0):
        deadline = time.monotonic() + timeout
        observed = []
        while time.monotonic() < deadline:
            try:
                record = self._records.get(timeout=min(0.1, deadline - time.monotonic()))
            except queue.Empty:
                continue
            if record.get("type") == "log":
                observed.append(record.get("message", ""))
                if text in observed[-1]:
                    return observed[-1]
            else:
                self._pending_records.append(record)
        raise AssertionError(f"missing log {text!r}; observed {observed!r}")

    def close(self):
        if self.process.poll() is not None:
            return
        try:
            shutdown = {"type": "shutdown", "reason": "test-complete", "deadline_ms": 1000}
            self.process.stdin.write(json.dumps(shutdown, separators=(",", ":")) + "\n")
            self.process.stdin.flush()
            self.process.wait(timeout=2)
        except (BrokenPipeError, subprocess.TimeoutExpired):
            self.process.kill()
            self.process.wait()


class AudioContract(unittest.TestCase):
    module_command = None
    fake_path = None
    supplied_environment = None
    supplied_init = None

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.bin_path = self.root / "bin"
        self.bin_path.mkdir()
        (self.bin_path / "pactl").symlink_to(self.fake_path / "fake-pactl")
        host_bin = self.root / "custom-prefix" / "bin"
        host_bin.mkdir(parents=True)
        host_command = list(self.module_command)
        copied_host = host_bin / "gisland-lua-host"
        shutil.copy2(host_command[0], copied_host)
        (host_bin / "gislandctl").symlink_to(self.fake_path / "fake-gislandctl")
        host_command[0] = str(copied_host)
        self.module_command = host_command
        self.state_path = self.root / "state.json"
        self.log_path = self.root / "commands.jsonl"
        self.module = None

    def tearDown(self):
        if self.module is not None:
            self.module.close()
        self.temporary.cleanup()

    def start(self, state, configuration=None, extra_environment=None):
        self.state_path.write_text(json.dumps(state), encoding="utf-8")
        environment = os.environ.copy()
        environment.update(
            {
                "PATH": f"{self.bin_path}:/usr/bin:/bin",
                "HOME": str(self.root),
                "GISLAND_AUDIO_FAKE_STATE": str(self.state_path),
                "GISLAND_AUDIO_COMMAND_LOG": str(self.log_path),
            }
        )
        environment.update(self.supplied_environment)
        environment.update(extra_environment or {})
        init = self.supplied_init or {
            "type": "init",
            "protocol": {
                "minimum": {"major": 1, "minor": 7},
                "maximum": {"major": 1, "minor": 8},
            },
            "capabilities": CAPABILITIES,
            "configuration": configuration or {},
        }
        init = dict(init)
        init["configuration"] = configuration or init.get("configuration", {})
        self.module = ModuleProcess(self.module_command, environment, init)
        self.assertEqual(self.module.ready["protocol_major"], 1)
        self.assertIn(self.module.ready["protocol_minor"], (7, 8))

    def command_log(self):
        if not self.log_path.exists():
            return []
        return [
            json.loads(line)
            for line in self.log_path.read_text(encoding="utf-8").splitlines()
        ]

    def assert_pactl(self, expected):
        observed = [
            entry for entry in self.command_log() if entry["program"] == "pactl"
        ]
        self.assertEqual([entry["argv"] for entry in observed], expected)
        self.assertTrue(observed)
        self.assertEqual({entry["lc_all"] for entry in observed}, {"C"})

    def assert_configuration_rejected(self, configuration, message):
        environment = os.environ.copy()
        environment.update(
            {
                "PATH": f"{self.bin_path}:/usr/bin:/bin",
                "HOME": str(self.root),
                "GISLAND_AUDIO_FAKE_STATE": str(self.state_path),
                "GISLAND_AUDIO_COMMAND_LOG": str(self.log_path),
            }
        )
        environment.update(self.supplied_environment)
        init = self.supplied_init or {
            "type": "init",
            "protocol": {
                "minimum": {"major": 1, "minor": 7},
                "maximum": {"major": 1, "minor": 8},
            },
            "capabilities": CAPABILITIES,
            "configuration": {},
        }
        init = dict(init)
        init["configuration"] = configuration
        completed = subprocess.run(
            self.module_command,
            input=json.dumps(init, separators=(",", ":")) + "\n",
            capture_output=True,
            text=True,
            encoding="utf-8",
            env=environment,
            check=False,
            timeout=4.0,
        )
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn(message, completed.stderr)

    @staticmethod
    def volume_publication(before, after, maximum, duration, icon):
        return {
            "type": "publish",
            "context_id": "audio-volume",
            "priority": 80,
            "expires_in_ms": duration,
            "views": {
                "compact": {
                    "type": "row",
                    "gap": "small",
                    "children": [
                        {
                            "type": "icon",
                            "name": icon,
                            "role": "hud-volume-icon",
                            "accessible_label": f"Volume {after} percent",
                        },
                        {
                            "type": "progress",
                            "value": max(0.0, min(after / maximum, 1.0)),
                            "transition_from": max(0.0, min(before / maximum, 1.0)),
                            "state": "foreground",
                        },
                    ],
                }
            },
            "presentation": {"compact_style": "hud-meter"},
        }

    @staticmethod
    def mute_publication(muted, duration):
        return {
            "type": "publish",
            "context_id": "audio-mute",
            "priority": 80,
            "expires_in_ms": duration,
            "views": {
                "compact": {
                    "type": "icon",
                    "name": "volume-muted" if muted else "volume-high",
                    "role": "hud-mute-icon",
                    "accessible_label": "Muted" if muted else "Unmuted",
                }
            },
            "presentation": {"compact_style": "hud-symbol"},
        }

    @staticmethod
    def accepted_result(action_id):
        return {"type": "action_result", "action_id": action_id, "accepted": True}

    def test_configuration_names_types_and_bounds(self):
        invalid = (
            ({"unknown": 1}, "unknown audio option: unknown"),
            ({"step_percent": True}, "step_percent must be an integer"),
            ({"step_percent": 0}, "step_percent must be between 1 and 25"),
            ({"step_percent": 26}, "step_percent must be between 1 and 25"),
            (
                {"maximum_percent": 99},
                "maximum_percent must be between 100 and 200",
            ),
            (
                {"maximum_percent": 201},
                "maximum_percent must be between 100 and 200",
            ),
            (
                {"hud_duration_ms": 99},
                "hud_duration_ms must be between 100 and 60000",
            ),
            (
                {"hud_duration_ms": 60001},
                "hud_duration_ms must be between 100 and 60000",
            ),
        )
        for configuration, message in invalid:
            with self.subTest(configuration=configuration):
                self.assert_configuration_rejected(configuration, message)

    def test_multiple_unknown_options_are_reported_in_sorted_order(self):
        self.assert_configuration_rejected(
            {"zeta": 1, "alpha": 2}, "unknown audio option: alpha"
        )

    def test_volume_contract_parses_channels_rounds_and_uses_authoritative_readback(self):
        self.start(
            {
                "volume_reads": [[40, 41], [47, 48]],
                "mute_reads": [True, False],
            },
            {"step_percent": 7, "maximum_percent": 180, "hud_duration_ms": 3210},
        )
        action = self.module.action("volume-up")
        self.assertTrue(action["accepted"])
        self.assertEqual(
            action["records"],
            [
                self.volume_publication(40, 48, 180, 3210, "volume-low"),
                self.accepted_result("volume-up"),
            ],
        )
        self.module.assert_no_record()
        self.assert_pactl(
            [
                ["get-sink-volume", "@DEFAULT_SINK@"],
                ["get-sink-mute", "@DEFAULT_SINK@"],
                ["set-sink-mute", "@DEFAULT_SINK@", "0"],
                ["set-sink-volume", "@DEFAULT_SINK@", "47%"],
                ["get-sink-volume", "@DEFAULT_SINK@"],
                ["get-sink-mute", "@DEFAULT_SINK@"],
            ]
        )

    def test_clamp_zero_icon_and_replacement_order(self):
        self.start(
            {
                "volume_reads": [[2], [0], [0], [0]],
                "mute_reads": [False, False, False, True],
            },
            {"step_percent": 5, "maximum_percent": 150, "hud_duration_ms": 1500},
        )
        volume_action = self.module.action("volume-down")
        self.assertTrue(volume_action["accepted"])
        self.assertEqual(
            volume_action["records"],
            [
                self.volume_publication(2, 0, 150, 1500, "volume-muted"),
                self.accepted_result("volume-down"),
            ],
        )
        mute_action = self.module.action("toggle-mute")
        self.assertTrue(mute_action["accepted"])
        self.assertEqual(
            mute_action["records"],
            [
                {"type": "dismiss", "context_id": "audio-volume"},
                self.mute_publication(True, 1500),
                self.accepted_result("toggle-mute"),
            ],
        )
        self.module.assert_no_record()
        self.assert_pactl(
            [
                ["get-sink-volume", "@DEFAULT_SINK@"],
                ["get-sink-mute", "@DEFAULT_SINK@"],
                ["set-sink-volume", "@DEFAULT_SINK@", "0%"],
                ["get-sink-volume", "@DEFAULT_SINK@"],
                ["get-sink-mute", "@DEFAULT_SINK@"],
                ["get-sink-volume", "@DEFAULT_SINK@"],
                ["get-sink-mute", "@DEFAULT_SINK@"],
                ["set-sink-mute", "@DEFAULT_SINK@", "1"],
                ["get-sink-volume", "@DEFAULT_SINK@"],
                ["get-sink-mute", "@DEFAULT_SINK@"],
            ]
        )

    def test_unmute_icon_and_maximum_clamp(self):
        self.start(
            {
                "volume_reads": [[199], [180], [180], [180]],
                "mute_reads": [False, False, True, False],
            },
            {"step_percent": 25, "maximum_percent": 180, "hud_duration_ms": 100},
        )
        volume_action = self.module.action("volume-up")
        self.assertTrue(volume_action["accepted"])
        self.assertEqual(
            volume_action["records"],
            [
                self.volume_publication(199, 180, 180, 100, "volume-high"),
                self.accepted_result("volume-up"),
            ],
        )
        mute_action = self.module.action("toggle-mute")
        self.assertTrue(mute_action["accepted"])
        self.assertEqual(
            mute_action["records"],
            [
                {"type": "dismiss", "context_id": "audio-volume"},
                self.mute_publication(False, 100),
                self.accepted_result("toggle-mute"),
            ],
        )
        self.module.assert_no_record()
        self.assertIn(
            ["set-sink-volume", "@DEFAULT_SINK@", "180%"],
            [entry["argv"] for entry in self.command_log()],
        )

    def test_pactl_failure_rejects_action_logs_and_module_remains_available(self):
        failing = ["get-sink-volume", "@DEFAULT_SINK@"]
        self.start(
            {
                "volume_reads": [[30], [35]],
                "mute_reads": [False, False],
                "fail_once": " ".join(failing),
                "failure_message": "sink disappeared",
            }
        )
        rejected = self.module.action("volume-up")
        self.assertFalse(rejected["accepted"])
        self.assertIn("pactl", rejected["log"])
        self.assertIsNone(self.module.process.poll())
        recovered = self.module.action("volume-up")
        self.assertTrue(recovered["accepted"])
        self.assertEqual(
            recovered["records"],
            [
                self.volume_publication(30, 35, 150, 1500, "volume-low"),
                self.accepted_result("volume-up"),
            ],
        )

    def test_pactl_and_gislandctl_timeouts_are_bounded_and_logged(self):
        get_volume = "get-sink-volume @DEFAULT_SINK@"
        self.start(
            {
                "volume_reads": [[20], [25]],
                "mute_reads": [False, False],
                "sleep_once": get_volume,
                "sleep_seconds": 3,
            },
            extra_environment={"GISLAND_AUDIO_FAKE_GISLANDCTL_SLEEP": "1"},
        )
        started = time.monotonic()
        rejected = self.module.action("volume-up")
        elapsed = time.monotonic() - started
        self.assertFalse(rejected["accepted"])
        self.assertIn("timed out after 2.0 seconds", rejected["log"])
        self.assertGreaterEqual(elapsed, 1.8)
        self.assertLess(elapsed, 2.8)
        recovered = self.module.action("volume-up")
        self.assertTrue(recovered["accepted"])
        self.assertEqual(
            recovered["records"],
            [
                self.volume_publication(20, 25, 150, 1500, "volume-low"),
                self.accepted_result("volume-up"),
            ],
        )
        self.module.wait_for_stderr("timed out after 0.5 seconds")
        closes = [
            entry for entry in self.command_log() if entry["program"] == "gislandctl"
        ]
        self.assertEqual(closes[-1]["argv"], ["close"])

    def test_lua_defers_close_until_publish_and_action_result_are_emitted(self):
        started_marker = self.root / "close-started"
        release_marker = self.root / "close-release"
        self.start(
            {"volume_reads": [[20], [25]], "mute_reads": [False, False]},
            extra_environment={
                "GISLAND_AUDIO_FAKE_GISLANDCTL_STARTED": str(started_marker),
                "GISLAND_AUDIO_FAKE_GISLANDCTL_RELEASE": str(release_marker),
            },
        )
        result_queue = queue.Queue()
        action_thread = threading.Thread(
            target=lambda: result_queue.put(self.module.action("volume-up")), daemon=True
        )
        action_thread.start()
        result = result_queue.get(timeout=2.0)
        self.assertTrue(result["accepted"])
        self.assertEqual(
            result["records"],
            [
                self.volume_publication(20, 25, 150, 1500, "volume-low"),
                self.accepted_result("volume-up"),
            ],
        )
        deadline = time.monotonic() + 1.0
        while not started_marker.exists() and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertTrue(started_marker.exists(), "deferred close did not start")
        release_marker.touch()
        action_thread.join(timeout=1.0)


def parse_command(value: str):
    command = json.loads(value)
    if not isinstance(command, list) or not command or not all(
        isinstance(part, str) and part for part in command
    ):
        raise argparse.ArgumentTypeError("command must be a JSON array of strings")
    return command


def parse_object(value: str):
    parsed = json.loads(value)
    if not isinstance(parsed, dict):
        raise argparse.ArgumentTypeError("value must be a JSON object")
    return parsed


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--module-command", type=parse_command)
    parser.add_argument("--host")
    parser.add_argument("--entry")
    parser.add_argument("--fake-path", type=Path)
    parser.add_argument("--environment", type=parse_object, default={})
    parser.add_argument("--init", type=parse_object)
    arguments, unittest_arguments = parser.parse_known_args()
    if arguments.module_command is None:
        parser.error("--module-command is required")
    replacements = {"host": arguments.host or "", "entry": arguments.entry or ""}
    AudioContract.module_command = [
        part.format_map(replacements) for part in arguments.module_command
    ]
    AudioContract.fake_path = (arguments.fake_path or Path(__file__).parent / "fixtures").resolve()
    AudioContract.supplied_environment = arguments.environment
    AudioContract.supplied_init = arguments.init
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(AudioContract)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
