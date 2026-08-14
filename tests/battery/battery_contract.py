#!/usr/bin/env python3

import argparse
from collections import deque
import json
import os
import queue
import stat
import subprocess
import tempfile
import threading
import time
import unittest
from pathlib import Path

import gi

gi.require_version("Gio", "2.0")
gi.require_version("GLib", "2.0")
from gi.repository import Gio, GLib


CAPABILITIES = [
    "data-snapshots",
    "rich-content",
    "independent-views",
    "ring-progress",
]
ROOT_PATH = "/org/freedesktop/UPower"
DEVICE_PATH = "/org/freedesktop/UPower/devices/DisplayDevice"
ROOT_INTERFACE = "org.freedesktop.UPower"
DEVICE_INTERFACE = "org.freedesktop.UPower.Device"

ROOT_XML = """
<node><interface name="org.freedesktop.UPower">
  <property name="OnBattery" type="b" access="read"/>
</interface></node>
"""
DEVICE_XML = """
<node><interface name="org.freedesktop.UPower.Device">
  <property name="Percentage" type="d" access="read"/>
  <property name="State" type="u" access="read"/>
  <property name="TimeToEmpty" type="x" access="read"/>
  <property name="TimeToFull" type="x" access="read"/>
  <property name="EnergyRate" type="d" access="read"/>
  <property name="EnergyFull" type="d" access="read"/>
  <property name="EnergyFullDesign" type="d" access="read"/>
  <property name="IsPresent" type="b" access="read"/>
</interface></node>
"""


class FakeUPower:
    signatures = {
        "OnBattery": "b",
        "Percentage": "d",
        "State": "u",
        "TimeToEmpty": "x",
        "TimeToFull": "x",
        "EnergyRate": "d",
        "EnergyFull": "d",
        "EnergyFullDesign": "d",
        "IsPresent": "b",
    }

    def __init__(self):
        address = os.environ["DBUS_SESSION_BUS_ADDRESS"]
        self.connection = Gio.DBusConnection.new_for_address_sync(
            address,
            Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT
            | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION,
            None,
            None,
        )
        self.connection.call_sync(
            "org.freedesktop.DBus",
            "/org/freedesktop/DBus",
            "org.freedesktop.DBus",
            "RequestName",
            GLib.Variant("(su)", ("org.freedesktop.UPower", 0)),
            GLib.VariantType("(u)"),
            Gio.DBusCallFlags.NONE,
            -1,
            None,
        )
        self.root = {"OnBattery": True}
        self.device = {
            "Percentage": 72.4,
            "State": 2,
            "TimeToEmpty": 3 * 3600 + 12 * 60,
            "TimeToFull": 0,
            "EnergyRate": 12.36,
            "EnergyFull": 44.0,
            "EnergyFullDesign": 50.0,
            "IsPresent": True,
        }
        self.registrations = [
            self.connection.register_object(
                ROOT_PATH,
                Gio.DBusNodeInfo.new_for_xml(ROOT_XML).interfaces[0],
                None,
                lambda _connection, _sender, _path, _interface, name: self._property(
                    self.root, name
                ),
                None,
            ),
            self.connection.register_object(
                DEVICE_PATH,
                Gio.DBusNodeInfo.new_for_xml(DEVICE_XML).interfaces[0],
                None,
                lambda _connection, _sender, _path, _interface, name: self._property(
                    self.device, name
                ),
                None,
            ),
        ]
        self.loop = GLib.MainLoop()
        self.thread = threading.Thread(target=self.loop.run, daemon=True)
        self.thread.start()

    def _property(self, values, name):
        return GLib.Variant(self.signatures[name], values[name])

    def change_root(self, **values):
        self.root.update(values)
        self._changed(ROOT_PATH, ROOT_INTERFACE, values)

    def change_device(self, **values):
        self.device.update(values)
        self._changed(DEVICE_PATH, DEVICE_INTERFACE, values)

    def _changed(self, path, interface, values):
        changed = {
            name: GLib.Variant(self.signatures[name], value)
            for name, value in values.items()
        }
        self.connection.emit_signal(
            None,
            path,
            "org.freedesktop.DBus.Properties",
            "PropertiesChanged",
            GLib.Variant("(sa{sv}as)", (interface, changed, [])),
        )
        self.connection.flush_sync(None)

    def close(self):
        self.loop.quit()
        self.thread.join(timeout=1.0)
        for registration in self.registrations:
            self.connection.unregister_object(registration)
        self.connection.close_sync(None)


def init_message(protocol_minor, configuration=None):
    return {
        "type": "init",
        "protocol": {
            "minimum": {"major": 1, "minor": protocol_minor},
            "maximum": {"major": 1, "minor": protocol_minor},
        },
        "instance_id": "battery-contract",
        "capabilities": CAPABILITIES,
        "configuration": configuration or {},
        "locale": "C",
        "timezone": "UTC",
    }


class ModuleProcess:
    def __init__(self, command, environment, init):
        self.records = queue.Queue()
        self.pending = deque()
        self.stderr = queue.Queue()
        self.invocation = 0
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
            self.records.put(json.loads(line))

    def _read_stderr(self):
        for line in self.process.stderr:
            self.stderr.put(line.rstrip("\n"))

    def send(self, record):
        self.process.stdin.write(json.dumps(record, separators=(",", ":")) + "\n")
        self.process.stdin.flush()

    def next_record(self, timeout=3.0):
        if self.pending:
            return self.pending.popleft()
        try:
            return self.records.get(timeout=timeout)
        except queue.Empty as error:
            diagnostics = []
            while not self.stderr.empty():
                diagnostics.append(self.stderr.get_nowait())
            raise AssertionError(
                f"timed out waiting for module output; exit={self.process.poll()}; "
                f"stderr={diagnostics!r}"
            ) from error

    def collect(self, quiet=0.15):
        result = []
        while True:
            try:
                result.append(self.records.get(timeout=quiet))
            except queue.Empty:
                return result

    def wait_for(self, predicate, timeout=3.0):
        deadline = time.monotonic() + timeout
        unmatched = []
        while time.monotonic() < deadline:
            record = self.next_record(timeout=max(0.01, deadline - time.monotonic()))
            if predicate(record):
                self.pending.extend(unmatched)
                return record
            unmatched.append(record)
        self.pending.extend(unmatched)
        raise AssertionError("timed out waiting for matching module output")

    def action(self, action_id):
        self.invocation += 1
        invocation = str(self.invocation)
        self.send(
            {"type": "action", "action_id": action_id, "invocation_id": invocation}
        )
        records = []
        while True:
            record = self.next_record()
            if record.get("type") == "action_result":
                self.assert_invocation(record, invocation)
                return records, record
            records.append(record)

    def assert_invocation(self, record, invocation):
        if "invocation_id" in record and record["invocation_id"] != invocation:
            raise AssertionError(f"wrong invocation ID: {record!r}")
        if BatteryContract.protocol_minor >= 8 and "invocation_id" not in record:
            raise AssertionError(f"missing invocation ID: {record!r}")

    def close(self):
        if self.process.poll() is None:
            try:
                self.send(
                    {"type": "shutdown", "reason": "test-complete", "deadline_ms": 1000}
                )
                self.process.wait(timeout=2.0)
            except (BrokenPipeError, subprocess.TimeoutExpired):
                self.process.kill()
                self.process.wait()


def snapshot(
    level,
    percent,
    semantic,
    compact,
    state,
    detail,
    health="—",
    power="—",
):
    return {
        "level": level,
        "percent_text": percent,
        "semantic_state": semantic,
        "estimate_compact": compact,
        "state_text": state,
        "estimate_detail": detail,
        "health_text": health,
        "power_text": power,
    }


def compact_view(value):
    return {
        "type": "row",
        "gap": "small",
        "children": [
            {
                "type": "progress",
                "shape": "ring",
                "value": value["level"],
                "state": value["semantic_state"],
            },
            {"type": "text", "value": value["percent_text"], "role": "compact-primary"},
            {"type": "spacer", "flexible": True},
            {
                "type": "text",
                "value": value["estimate_compact"],
                "role": "compact-secondary",
            },
        ],
    }


def expanded_view(value, dismissible):
    rows = [
        {
            "type": "row",
            "gap": "normal",
            "children": [
                {
                    "type": "progress",
                    "shape": "ring",
                    "value": value["level"],
                    "state": value["semantic_state"],
                },
                {"type": "text", "value": value["percent_text"], "role": "title"},
                {"type": "spacer", "flexible": True},
                {"type": "text", "value": value["state_text"], "role": "body"},
            ],
        }
    ]
    for label, key in (
        ("Autonomie", "estimate_detail"),
        ("Santé", "health_text"),
        ("Puissance", "power_text"),
    ):
        rows.append(
            {
                "type": "row",
                "gap": "small",
                "children": [
                    {"type": "text", "value": label, "role": "caption"},
                    {"type": "spacer", "flexible": True},
                    {"type": "text", "value": value[key], "role": "body"},
                ],
            }
        )
    if dismissible:
        rows.append(
            {
                "type": "button",
                "action_id": "dismiss-alert",
                "accessible_label": "Dismiss battery alert",
                "content": {"type": "text", "value": "Dismiss", "role": "button"},
            }
        )
    return {"type": "column", "gap": "normal", "children": rows}


def publication(kind, value, duration=3000):
    persistent = kind in ("persistent", "critical")
    priorities = {
        "plugged": 35,
        "unplugged": 35,
        "warning": 40,
        "persistent": 60,
        "critical": 90,
    }
    record = {
        "type": "publish",
        "context_id": f"battery-{kind}",
        "priority": priorities[kind],
        "views": {
            "compact": compact_view(value),
            "expanded": expanded_view(value, persistent),
        },
        "presentation": {"reveal": "expanded"},
    }
    if not persistent:
        record["expires_in_ms"] = duration
        record["presentation"]["duration_ms"] = duration
    return record


class BatteryContract(unittest.TestCase):
    command = None
    protocol_minor = None

    @classmethod
    def setUpClass(cls):
        cls.upower = FakeUPower()

    @classmethod
    def tearDownClass(cls):
        cls.upower.close()

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.module = None
        self.upower.root = {"OnBattery": True}
        self.upower.device = {
            "Percentage": 72.4,
            "State": 2,
            "TimeToEmpty": 3 * 3600 + 12 * 60,
            "TimeToFull": 0,
            "EnergyRate": 12.36,
            "EnergyFull": 44.0,
            "EnergyFullDesign": 50.0,
            "IsPresent": True,
        }

    def tearDown(self):
        if self.module is not None:
            self.module.close()
        self.temporary.cleanup()

    def environment(self):
        environment = os.environ.copy()
        environment["DBUS_SYSTEM_BUS_ADDRESS"] = environment["DBUS_SESSION_BUS_ADDRESS"]
        environment["XDG_STATE_HOME"] = str(self.root / "state")
        return environment

    def start(self, configuration=None):
        self.module = ModuleProcess(
            self.command,
            self.environment(),
            init_message(self.protocol_minor, configuration),
        )
        self.assertEqual(self.module.ready["protocol_major"], 1)
        self.assertEqual(self.module.ready["protocol_minor"], self.protocol_minor)
        self.assertEqual(self.module.ready["capabilities"], CAPABILITIES)
        return self.module.next_record()

    def restart(self, configuration=None):
        self.module.close()
        self.module = None
        return self.start(configuration)

    def change_device(self, **values):
        self.upower.change_device(**values)
        return self.module.collect()

    def change_root(self, **values):
        self.upower.change_root(**values)
        return self.module.collect()

    def assert_rejected(self, configuration, message):
        completed = subprocess.run(
            self.command,
            input=json.dumps(
                init_message(self.protocol_minor, configuration), separators=(",", ":")
            )
            + "\n",
            capture_output=True,
            text=True,
            encoding="utf-8",
            env=self.environment(),
            check=False,
            timeout=3.0,
        )
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn(message, completed.stderr)

    def test_exact_initial_snapshot_includes_every_measurement(self):
        self.assertEqual(
            self.start(),
            {
                "type": "data",
                "value": snapshot(
                    0.724,
                    "72 %",
                    "success",
                    "3 h 12",
                    "Décharge",
                    "3 h 12 restantes",
                    "88 %",
                    "12,4 W",
                ),
            },
        )

    def test_percentage_normalization_half_even_rounding_and_semantic_boundaries(self):
        self.upower.root["OnBattery"] = False
        self.upower.device.update(State=1, TimeToEmpty=0, TimeToFull=0, EnergyRate=0.0)
        self.start()
        cases = (
            (-2.0, 0.0, "0 %", "error"),
            (2.5, 0.025, "2 %", "error"),
            (15.0, 0.15, "15 %", "error"),
            (15.1, 0.151, "15 %", "warning"),
            (30.0, 0.3, "30 %", "warning"),
            (30.1, 0.301, "30 %", "success"),
            (72.5, 0.725, "72 %", "success"),
            (100.6, 1.0, "100 %", "success"),
        )
        for percentage, level, text, semantic in cases:
            with self.subTest(percentage=percentage):
                records = self.change_device(Percentage=percentage)
                self.assertEqual(len(records), 1)
                value = records[0]["value"]
                self.assertEqual(
                    (value["level"], value["percent_text"], value["semantic_state"]),
                    (level, text, semantic),
                )

    def test_all_upower_states_estimates_health_power_and_present_false(self):
        self.upower.root["OnBattery"] = False
        self.start()
        state_cases = (
            (0, False, "Secteur"),
            (1, False, "Charge"),
            (2, True, "Décharge"),
            (3, True, "Décharge"),
            (4, False, "Chargée"),
            (5, False, "Secteur"),
            (6, True, "Décharge"),
            (77, False, "Secteur"),
        )
        percentage = 70.0
        for state, on_battery, expected in state_cases:
            percentage += 1.0
            self.upower.root["OnBattery"] = on_battery
            self.upower.device["State"] = state
            self.upower.change_root(OnBattery=on_battery)
            self.module.collect()
            records = self.change_device(State=state, Percentage=percentage)
            self.assertEqual(records[0]["value"]["state_text"], expected)

        records = self.change_device(
            Percentage=80.0,
            State=1,
            TimeToFull=3900,
            EnergyRate=0.04,
            EnergyFull=0.0,
            EnergyFullDesign=50.0,
        )
        value = records[0]["value"]
        self.assertEqual(value["estimate_compact"], "1 h 05")
        self.assertEqual(value["estimate_detail"], "1 h 05 avant charge complète")
        self.assertEqual(value["health_text"], "—")
        self.assertEqual(value["power_text"], "0,0 W")
        self.module.collect()
        self.change_device(IsPresent=False, Percentage=81.0)
        self.assertEqual(self.module.collect(), [])
        self.assertEqual(
            self.change_device(IsPresent=True, Percentage=float("nan")), []
        )
        self.assertEqual(self.change_device(Percentage=float("inf")), [])
        recovered = self.change_device(Percentage=82.0)
        self.assertEqual(recovered[0]["value"]["percent_text"], "82 %")

    def test_estimate_and_measurement_only_changes_are_coalesced(self):
        self.start()
        self.assertEqual(self.change_device(TimeToEmpty=7140), [])
        self.assertEqual(self.change_device(EnergyRate=9.5), [])

    def test_alert_priorities_expiration_replacement_dismissal_and_suppression(self):
        self.upower.root["OnBattery"] = False
        initial = self.start({"preview_duration_ms": 2345})["value"]
        self.upower.change_root(OnBattery=True)
        records = self.module.collect()
        unplugged_value = dict(
            initial,
            state_text="Décharge",
            estimate_compact="3 h 12",
            estimate_detail="3 h 12 restantes",
        )
        self.assertEqual(
            records,
            [
                {"type": "data", "value": unplugged_value},
                publication("unplugged", unplugged_value, 2345),
            ],
        )

        records = self.change_device(Percentage=19.9, State=2)
        warning_value = records[0]["value"]
        self.assertEqual(records[1:], [publication("warning", warning_value, 2345)])
        self.assertEqual(self.change_device(Percentage=19.0), [{"type": "data", "value": dict(warning_value, level=0.19, percent_text="19 %")}])

        records = self.change_device(Percentage=9.9)
        persistent_value = records[0]["value"]
        self.assertEqual(records[1:], [publication("persistent", persistent_value, 2345)])
        records, result = self.module.action("dismiss-alert")
        self.assertEqual(records, [{"type": "dismiss", "context_id": "battery-persistent"}])
        self.assertTrue(result["accepted"])
        records, result = self.module.action("dismiss-alert")
        self.assertEqual(records, [])
        self.assertFalse(result["accepted"])

        records = self.change_device(Percentage=4.9)
        critical_value = records[0]["value"]
        self.assertEqual(records[1:], [publication("critical", critical_value, 2345)])
        duplicate_records = self.change_device(Percentage=4.8)
        self.assertEqual(duplicate_records, [])
        latest_value = dict(critical_value, level=0.048)

        self.upower.change_root(OnBattery=False)
        records = self.module.collect()
        self.assertEqual(
            records,
            [
                {
                    "type": "data",
                    "value": dict(
                        latest_value,
                        state_text="Secteur",
                        estimate_compact="Calcul…",
                        estimate_detail="—",
                    ),
                },
                {"type": "dismiss", "context_id": "battery-critical"},
                publication(
                    "plugged",
                    dict(
                        latest_value,
                        state_text="Secteur",
                        estimate_compact="Calcul…",
                        estimate_detail="—",
                    ),
                    2345,
                ),
            ],
        )

        self.upower.change_root(OnBattery=True)
        records = self.module.collect()
        recovered_value = dict(
            latest_value,
            state_text="Décharge",
            estimate_compact="3 h 12",
            estimate_detail="3 h 12 restantes",
        )
        self.assertEqual(
            records,
            [
                {"type": "data", "value": recovered_value},
                publication("unplugged", recovered_value, 2345),
                publication("critical", recovered_value, 2345),
            ],
        )
        records, result = self.module.action("dismiss-alert")
        self.assertEqual(records, [{"type": "dismiss", "context_id": "battery-critical"}])
        self.assertTrue(result["accepted"])
        records, result = self.module.action("dismiss-alert")
        self.assertEqual(records, [])
        self.assertFalse(result["accepted"])

    def test_startup_low_state_persists_all_crossed_thresholds_and_suppresses_duplicates(self):
        self.upower.device["Percentage"] = 9.0
        records = [self.start(), *self.module.collect()]
        value = records[0]["value"]
        self.assertEqual(records[1:], [publication("persistent", value)])
        state_path = self.root / "state" / "gisland" / "battery-cycle.json"
        state = json.loads(
            state_path.read_text(encoding="utf-8")
        )
        self.assertEqual(state, {"version": 1, "emitted": [10, 20], "previous_on_battery": True})
        self.assertEqual(state_path.stat().st_mode & 0o777, 0o600)
        records = [self.restart(), *self.module.collect()]
        self.assertEqual([record["type"] for record in records], ["data"])

    def test_corrupt_cycle_state_is_ignored_and_replaced(self):
        state_path = self.root / "state" / "gisland" / "battery-cycle.json"
        state_path.parent.mkdir(parents=True)
        state_path.write_text("not json\n", encoding="utf-8")
        self.upower.device["Percentage"] = 9.0
        records = [self.start(), *self.module.collect()]
        self.assertEqual(records[1]["context_id"], "battery-persistent")
        self.assertEqual(
            json.loads(state_path.read_text(encoding="utf-8"))["emitted"], [10, 20]
        )

    def test_cycle_state_requires_the_exact_strict_schema(self):
        invalid_states = (
            '[1,2,3]',
            '{"version":true,"emitted":[],"previous_on_battery":false}',
            '{"version":1.0,"emitted":[],"previous_on_battery":false}',
            '{"version":1,"emitted":[20.0],"previous_on_battery":false}',
            '{"version":1,"emitted":[true],"previous_on_battery":false}',
            '{"version":1,"emitted":[19],"previous_on_battery":false}',
            '{"version":1,"emitted":[],"previous_on_battery":0}',
            '{"version":1,"emitted":[]}',
            '{"version":1,"emitted":[],"previous_on_battery":false,"extra":1}',
            '{"version":1,"emitted":[],"previous_on_battery":false} trailing',
        )
        state_path = self.root / "state" / "gisland" / "battery-cycle.json"
        state_path.parent.mkdir(parents=True)
        self.upower.device["Percentage"] = 9.0
        for value in invalid_states:
            with self.subTest(value=value):
                state_path.write_text(value + "\n", encoding="utf-8")
                records = [self.start(), *self.module.collect()]
                self.assertEqual(records[1]["context_id"], "battery-persistent")
                self.assertEqual(
                    json.loads(state_path.read_text(encoding="utf-8")),
                    {
                        "version": 1,
                        "emitted": [10, 20],
                        "previous_on_battery": True,
                    },
                )
                self.module.close()
                self.module = None

    def test_cycle_state_accepts_only_configured_thresholds(self):
        state_path = self.root / "state" / "gisland" / "battery-cycle.json"
        state_path.parent.mkdir(parents=True)
        state_path.write_text(
            '{"version":1,"emitted":[20],"previous_on_battery":false}\n',
            encoding="utf-8",
        )
        self.upower.device["Percentage"] = 9.0
        records = [
            self.start(
                {
                    "warning_percent": 25,
                    "persistent_percent": 12,
                    "critical_percent": 6,
                }
            ),
            *self.module.collect(),
        ]
        self.assertEqual(records[1]["context_id"], "battery-persistent")
        self.assertEqual(
            json.loads(state_path.read_text(encoding="utf-8"))["emitted"], [12, 25]
        )

    def test_state_replacement_ignores_predictable_symlink_and_stale_temps(self):
        state_directory = self.root / "state" / "gisland"
        state_directory.mkdir(parents=True)
        former_target = self.root / "former-target"
        former_target.write_text("do not replace\n", encoding="utf-8")
        former_temp = state_directory / "battery-cycle.json.tmp"
        former_temp.symlink_to(former_target)
        stale_temp = state_directory / ".battery-cycle-stale"
        stale_temp.write_text("stale\n", encoding="utf-8")

        self.upower.device["Percentage"] = 9.0
        self.start()

        state_path = state_directory / "battery-cycle.json"
        self.assertEqual(former_target.read_text(encoding="utf-8"), "do not replace\n")
        self.assertTrue(former_temp.is_symlink())
        self.assertEqual(stale_temp.read_text(encoding="utf-8"), "stale\n")
        self.assertEqual(stat.S_IMODE(state_path.stat().st_mode), 0o600)
        self.assertEqual(
            json.loads(state_path.read_text(encoding="utf-8")),
            {"version": 1, "emitted": [10, 20], "previous_on_battery": True},
        )

    def test_exact_state_reload_replaces_overly_permissive_mode(self):
        state_path = self.root / "state" / "gisland" / "battery-cycle.json"
        state_path.parent.mkdir(parents=True)
        state_path.write_text(
            '{"version":1,"emitted":[20],"previous_on_battery":true}\n',
            encoding="utf-8",
        )
        state_path.chmod(0o644)
        self.upower.device["Percentage"] = 19.0

        records = [self.start(), *self.module.collect()]

        self.assertEqual([record["type"] for record in records], ["data"])
        self.assertEqual(stat.S_IMODE(state_path.stat().st_mode), 0o600)
        self.assertEqual(
            json.loads(state_path.read_text(encoding="utf-8")),
            {"version": 1, "emitted": [20], "previous_on_battery": True},
        )

    def test_state_permission_failure_is_reported_and_leaves_no_temp(self):
        state_directory = self.root / "state" / "gisland"
        state_directory.mkdir(parents=True)
        state_directory.chmod(0o500)
        try:
            self.start()
            error = self.module.wait_for(
                lambda record: record.get("type") == "log"
                and "cannot save battery state" in record.get("message", "")
            )
            self.assertEqual(error["level"], "error")
            self.assertEqual(list(state_directory.iterdir()), [])
        finally:
            state_directory.chmod(0o700)

    def test_state_write_failure_is_reported_without_partial_or_temp_files(self):
        state_directory = self.root / "state" / "gisland"
        state_directory.mkdir(parents=True)
        state_path = state_directory / "battery-cycle.json"
        state_path.mkdir()

        self.start()
        error = self.module.wait_for(
            lambda record: record.get("type") == "log"
            and "cannot save battery state" in record.get("message", "")
        )
        self.assertEqual(error["level"], "error")
        self.assertTrue(state_path.is_dir())
        self.assertEqual(
            sorted(path.name for path in state_directory.iterdir()),
            ["battery-cycle.json"],
        )

    def test_invalid_options_are_rejected_deterministically(self):
        invalid = (
            ({"unknown": 1}, "unknown battery option: unknown"),
            ({"warning_percent": True}, "warning_percent must be an integer"),
            (
                {"critical_percent": 20, "persistent_percent": 10},
                "critical_percent, persistent_percent and warning_percent must satisfy "
                "0 < critical_percent < persistent_percent < warning_percent <= 100",
            ),
            (
                {"critical_percent": 10, "persistent_percent": 10},
                "critical_percent, persistent_percent and warning_percent must satisfy "
                "0 < critical_percent < persistent_percent < warning_percent <= 100",
            ),
            (
                {"persistent_percent": 20, "warning_percent": 20},
                "critical_percent, persistent_percent and warning_percent must satisfy "
                "0 < critical_percent < persistent_percent < warning_percent <= 100",
            ),
            (
                {"red_percent": 40, "yellow_percent": 30},
                "red_percent and yellow_percent must satisfy "
                "0 < red_percent < yellow_percent <= 100",
            ),
            (
                {"red_percent": 30, "yellow_percent": 30},
                "red_percent and yellow_percent must satisfy "
                "0 < red_percent < yellow_percent <= 100",
            ),
            (
                {"preview_duration_ms": 60001},
                "preview_duration_ms must be between 0 and 60000",
            ),
        )
        for configuration, message in invalid:
            with self.subTest(configuration=configuration):
                self.assert_rejected(configuration, message)
        self.assert_rejected(
            {"zeta": 1, "alpha": 1}, "unknown battery option: alpha"
        )

    def test_source_failure_is_logged_but_protocol_remains_available(self):
        environment = self.environment()
        environment["DBUS_SYSTEM_BUS_ADDRESS"] = "unix:path=/nonexistent"
        module = ModuleProcess(
            self.command,
            environment,
            init_message(self.protocol_minor),
        )
        try:
            self.assertEqual(module.ready["type"], "ready")
            deadline = time.monotonic() + 2.0
            messages = []
            while time.monotonic() < deadline:
                try:
                    record = module.records.get(timeout=0.1)
                except queue.Empty:
                    continue
                if record.get("type") == "log":
                    messages.append(record.get("message", ""))
                    if "UPower unavailable" in messages[-1]:
                        break
            if not any("UPower unavailable" in message for message in messages):
                while not module.stderr.empty():
                    messages.append(module.stderr.get_nowait())
            self.assertTrue(any("UPower unavailable" in message for message in messages), messages)
        finally:
            module.close()
        self.assertEqual(module.process.returncode, 0)


def parse_command(value):
    command = json.loads(value)
    if not isinstance(command, list) or not command:
        raise argparse.ArgumentTypeError("command must be a non-empty JSON array")
    return command


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--module-command", type=parse_command, required=True)
    parser.add_argument("--protocol-minor", type=int, required=True)
    arguments, unittest_arguments = parser.parse_known_args()
    BatteryContract.command = arguments.module_command
    BatteryContract.protocol_minor = arguments.protocol_minor
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(BatteryContract)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
