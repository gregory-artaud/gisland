#!/usr/bin/env python3

import argparse
import json
import os
import queue
import subprocess
import tempfile
import threading
import unittest
from datetime import datetime, timezone
from pathlib import Path


def unix_seconds(value: str) -> int:
    return int(datetime.fromisoformat(value).replace(tzinfo=timezone.utc).timestamp())


def init_message(configuration=None):
    return {
        "type": "init",
        "protocol": {
            "minimum": {"major": 1, "minor": 1},
            "maximum": {"major": 1, "minor": 8},
        },
        "instance_id": "clock-contract",
        "capabilities": ["data-snapshots"],
        "configuration": configuration or {},
        "locale": "C",
        "timezone": "UTC",
    }


class ModuleProcess:
    def __init__(self, command, environment, init):
        self._records = queue.Queue()
        self._stderr = queue.Queue()
        self._invocation = 0
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
        self.send(init)
        self.ready = self.next_record()
        if self.ready.get("type") != "ready":
            raise AssertionError(f"expected ready, got {self.ready!r}")

    def _read_stdout(self):
        for line in self.process.stdout:
            self._records.put(json.loads(line))

    def _read_stderr(self):
        for line in self.process.stderr:
            self._stderr.put(line.rstrip("\n"))

    def send(self, record):
        self.process.stdin.write(json.dumps(record, separators=(",", ":")) + "\n")
        self.process.stdin.flush()

    def next_record(self, timeout=3.0):
        try:
            return self._records.get(timeout=timeout)
        except queue.Empty as error:
            diagnostics = []
            while not self._stderr.empty():
                diagnostics.append(self._stderr.get_nowait())
            raise AssertionError(
                f"timed out waiting for module output; exit={self.process.poll()}; "
                f"stderr={diagnostics!r}"
            ) from error

    def snapshot(self):
        record = self.next_record()
        if record.get("type") != "data":
            raise AssertionError(f"expected data, got {record!r}")
        return record["value"]

    def action(self, action_id):
        self._invocation += 1
        invocation_id = str(self._invocation)
        self.send(
            {
                "type": "action",
                "action_id": action_id,
                "invocation_id": invocation_id,
            }
        )
        result = self.next_record()
        while result.get("type") == "log":
            result = self.next_record()
        if result.get("type") != "action_result":
            raise AssertionError(f"expected action_result, got {result!r}")
        normalized = dict(result)
        normalized.pop("invocation_id", None)
        expected = {
            "type": "action_result",
            "action_id": action_id,
            "accepted": action_id in {"previous-month", "next-month", "today"},
        }
        if normalized != expected:
            raise AssertionError(f"unexpected action result: {result!r}")
        if "invocation_id" in result and result["invocation_id"] != invocation_id:
            raise AssertionError(f"wrong invocation correlation: {result!r}")
        if self.ready["protocol_minor"] >= 8 and "invocation_id" not in result:
            raise AssertionError(f"missing invocation correlation: {result!r}")
        return result["accepted"]

    def close(self):
        if self.process.poll() is None:
            try:
                self.send(
                    {
                        "type": "shutdown",
                        "reason": "contract-complete",
                        "deadline_ms": 1000,
                    }
                )
                self.process.wait(timeout=2.0)
            except (BrokenPipeError, subprocess.TimeoutExpired):
                self.process.kill()
                self.process.wait()
        self.process.stdin.close()
        self.process.stdout.close()
        self.process.stderr.close()


class ClockCalendarContract(unittest.TestCase):
    command = None
    supplied_environment = None

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.now_path = Path(self.temporary.name) / "now"
        self.module = None

    def tearDown(self):
        if self.module is not None:
            self.module.close()
        self.temporary.cleanup()

    def start(self, now, configuration=None, init=None):
        self.set_now(now)
        environment = os.environ.copy()
        environment.update(self.supplied_environment)
        environment["GISLAND_CLOCK_CALENDAR_TEST_NOW_FILE"] = str(self.now_path)
        self.module = ModuleProcess(
            self.command,
            environment,
            init or init_message(configuration),
        )
        self.assertEqual(self.module.ready["protocol_major"], 1)
        self.assertIn(self.module.ready["protocol_minor"], (1, 8))
        self.assertEqual(self.module.ready["capabilities"], ["data-snapshots"])

    def set_now(self, value):
        self.now_path.write_text(str(unix_seconds(value)) + "\n", encoding="ascii")

    def assert_init_rejected(self, configuration):
        self.set_now("2026-08-03T14:35:42")
        environment = os.environ.copy()
        environment.update(self.supplied_environment)
        environment["GISLAND_CLOCK_CALENDAR_TEST_NOW_FILE"] = str(self.now_path)
        completed = subprocess.run(
            self.command,
            input=json.dumps(init_message(configuration), separators=(",", ":")) + "\n",
            capture_output=True,
            text=True,
            encoding="utf-8",
            env=environment,
            check=False,
            timeout=3.0,
        )
        self.assertNotEqual(completed.returncode, 0)

    def assert_snapshot_shape(self, snapshot):
        self.assertEqual(
            set(snapshot),
            {
                "time",
                "date_short",
                "month_label",
                "weekdays",
                "weeks",
                "calendar_columns",
            },
        )
        self.assertEqual(len(snapshot["weekdays"]), 7)
        self.assertEqual(len(snapshot["weeks"]), 6)
        self.assertTrue(all(len(week) == 7 for week in snapshot["weeks"]))
        self.assertEqual(len(snapshot["calendar_columns"]), 7)
        for index, column in enumerate(snapshot["calendar_columns"]):
            self.assertEqual(column["weekday"], snapshot["weekdays"][index])
            self.assertEqual(
                column["days"], [week[index] for week in snapshot["weeks"]]
            )

    def test_monday_grid_roles_and_navigation(self):
        self.start("2026-08-03T14:35:42")
        snapshot = self.module.snapshot()
        self.assert_snapshot_shape(snapshot)
        self.assertEqual(snapshot["time"], "14:35")
        self.assertEqual(snapshot["date_short"], "Mon 3 Aug")
        self.assertEqual(snapshot["month_label"], "August 2026")
        self.assertEqual(snapshot["weekdays"], ["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"])
        self.assertEqual(snapshot["weeks"][0][0], {"label": "27", "role": "muted"})
        self.assertEqual(snapshot["weeks"][0][5], {"label": "01", "role": "body"})
        self.assertEqual(snapshot["weeks"][1][0], {"label": "03", "role": "accent"})
        self.assertEqual(snapshot["weeks"][5][6], {"label": "06", "role": "muted"})

        self.assertTrue(self.module.action("previous-month"))
        self.assertEqual(self.module.snapshot()["month_label"], "July 2026")
        self.assertTrue(self.module.action("next-month"))
        self.assertEqual(self.module.snapshot()["month_label"], "August 2026")
        self.assertTrue(self.module.action("today"))
        self.assertEqual(self.module.snapshot()["month_label"], "August 2026")
        self.assertFalse(self.module.action("unknown"))

    def test_sunday_grid_and_timezone_conversion(self):
        self.start(
            "2026-08-03T22:35:00",
            {"locale": "C", "timezone": "Etc/GMT-2", "week_start": "sunday"},
        )
        snapshot = self.module.snapshot()
        self.assertEqual(snapshot["time"], "00:35")
        self.assertEqual(snapshot["date_short"], "Tue 4 Aug")
        self.assertEqual(snapshot["weekdays"][0], "Sun")
        self.assertEqual(snapshot["weeks"][0][0], {"label": "26", "role": "muted"})

    def test_locale_and_leap_year(self):
        self.start(
            "2024-02-29T08:05:00",
            {"locale": "fr_FR.UTF-8", "timezone": "UTC", "week_start": "monday"},
        )
        snapshot = self.module.snapshot()
        self.assertEqual(snapshot["date_short"], "jeu. 29 févr.")
        self.assertEqual(snapshot["month_label"], "février 2024")
        self.assertEqual(snapshot["weekdays"][0], "lun.")
        self.assertEqual(snapshot["weeks"][4][3], {"label": "29", "role": "accent"})

    def test_timezone_observes_daylight_saving_transition(self):
        self.start(
            "2026-03-08T06:59:42",
            {"locale": "C", "timezone": "America/New_York", "week_start": "sunday"},
        )
        snapshot = self.module.snapshot()
        self.assertEqual(snapshot["time"], "01:59")
        self.set_now("2026-03-08T07:00:00")
        self.module.send({"type": "visibility", "visibility": "compact-active"})
        self.assertEqual(self.module.snapshot()["time"], "03:00")

    def test_invalid_locale_timezone_and_week_start_fail_initialization(self):
        for configuration in (
            {"locale": "not-a-locale"},
            {"locale": ""},
            {"timezone": "not/a-zone"},
            {"timezone": ""},
            {"week_start": "tuesday"},
        ):
            with self.subTest(configuration=configuration):
                self.assert_init_rejected(configuration)

    def test_injected_minute_change_updates_time_date_and_followed_month(self):
        self.start("2026-08-31T23:59:42")
        self.assertEqual(self.module.snapshot()["month_label"], "August 2026")
        self.set_now("2026-09-01T00:00:00")
        self.module.send({"type": "visibility", "visibility": "compact-active"})
        snapshot = self.module.snapshot()
        self.assertEqual(snapshot["time"], "00:00")
        self.assertEqual(snapshot["date_short"], "Tue 1 Sep")
        self.assertEqual(snapshot["month_label"], "September 2026")
        self.assertEqual(snapshot["weeks"][0][1], {"label": "01", "role": "accent"})

    def test_navigation_persists_across_injected_minute_updates(self):
        self.start("2026-08-31T23:59:42")
        self.module.snapshot()
        self.assertTrue(self.module.action("previous-month"))
        self.assertEqual(self.module.snapshot()["month_label"], "July 2026")
        self.set_now("2026-09-01T00:00:00")
        self.module.send({"type": "visibility", "visibility": "expanded-active"})
        self.assertEqual(self.module.snapshot()["month_label"], "July 2026")
        self.assertTrue(self.module.action("today"))
        self.assertEqual(self.module.snapshot()["month_label"], "September 2026")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--module-command", required=True)
    parser.add_argument("--environment", default="{}")
    arguments, remaining = parser.parse_known_args()
    ClockCalendarContract.command = json.loads(arguments.module_command)
    ClockCalendarContract.supplied_environment = json.loads(arguments.environment)
    unittest.main(argv=[__file__, *remaining])


if __name__ == "__main__":
    main()
