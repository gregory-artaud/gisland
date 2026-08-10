import json
import os
import select
import subprocess
import tempfile
import unittest
from pathlib import Path

from gisland_battery.model import (
    AlertKind,
    AlertState,
    BatteryOptions,
    BatteryReading,
    CycleStateStore,
    snapshot_for,
)
from gisland_battery.service import BatteryService
from gisland_battery.protocol import ProtocolController


class BatteryModelTests(unittest.TestCase):
    def test_protocol_negotiates_all_capabilities_used_by_battery_contexts(self):
        records = []
        configurations = []
        actions = []
        failures = []
        controller = ProtocolController(
            records.append,
            configurations.append,
            lambda: None,
            lambda action_id: actions.append(action_id) is None,
            lambda: None,
            failures.append,
        )

        controller.handle(
            {
                "type": "init",
                "protocol": {
                    "minimum": {"major": 1, "minor": 5},
                    "maximum": {"major": 1, "minor": 5},
                },
                "capabilities": [
                    "data-snapshots",
                    "rich-content",
                    "independent-views",
                    "ring-progress",
                ],
                "configuration": {},
            }
        )

        self.assertEqual(failures, [])
        self.assertEqual(configurations, [{}])
        self.assertEqual(records[0]["protocol_minor"], 5)
        self.assertEqual(
            records[0]["capabilities"],
            [
                "data-snapshots",
                "rich-content",
                "independent-views",
                "ring-progress",
            ],
        )

    def test_snapshot_formats_level_estimate_health_and_power(self):
        reading = BatteryReading(
            percentage=72.4,
            on_battery=True,
            state="discharging",
            time_to_empty=3 * 3600 + 12 * 60,
            energy_rate=12.36,
            energy_full=44.0,
            energy_full_design=50.0,
        )

        self.assertEqual(
            snapshot_for(reading, BatteryOptions()),
            {
                "level": 0.724,
                "percent_text": "72 %",
                "semantic_state": "success",
                "estimate_compact": "3 h 12",
                "state_text": "Décharge",
                "estimate_detail": "3 h 12 restantes",
                "health_text": "88 %",
                "power_text": "12,4 W",
            },
        )

    def test_snapshot_handles_charging_and_unknown_estimates(self):
        charging = BatteryReading(25.0, False, "charging", time_to_full=3900)
        unknown = BatteryReading(10.0, True, "discharging")

        self.assertEqual(snapshot_for(charging, BatteryOptions())["estimate_compact"], "1 h 05")
        self.assertEqual(
            snapshot_for(charging, BatteryOptions())["estimate_detail"],
            "1 h 05 avant charge complète",
        )
        self.assertEqual(snapshot_for(unknown, BatteryOptions())["estimate_compact"], "Calcul…")
        self.assertEqual(snapshot_for(unknown, BatteryOptions())["estimate_detail"], "—")

    def test_options_validate_ordered_thresholds(self):
        options = BatteryOptions.from_mapping(
            {
                "warning_percent": 25,
                "persistent_percent": 12,
                "critical_percent": 6,
                "yellow_percent": 35,
                "red_percent": 18,
                "preview_duration_ms": 2500,
            }
        )
        self.assertEqual(options.warning_percent, 25)
        with self.assertRaisesRegex(ValueError, "critical_percent"):
            BatteryOptions.from_mapping({"critical_percent": 20, "persistent_percent": 10})

    def test_alerts_cover_power_changes_and_unique_threshold_crossings(self):
        alerts = AlertState(BatteryOptions())
        self.assertEqual(alerts.observe(BatteryReading(55.0, False, "charging")), [])
        self.assertEqual(
            alerts.observe(BatteryReading(55.0, True, "discharging")),
            [AlertKind.unplugged],
        )
        self.assertEqual(
            alerts.observe(BatteryReading(19.9, True, "discharging")),
            [AlertKind.warning],
        )
        self.assertEqual(alerts.observe(BatteryReading(19.0, True, "discharging")), [])
        self.assertEqual(
            alerts.observe(BatteryReading(9.9, True, "discharging")),
            [AlertKind.persistent],
        )
        self.assertEqual(
            alerts.observe(BatteryReading(4.9, True, "discharging")),
            [AlertKind.critical],
        )
        self.assertEqual(
            alerts.observe(BatteryReading(6.0, False, "charging")),
            [AlertKind.plugged],
        )

    def test_startup_emits_only_the_highest_unannounced_low_alert(self):
        alerts = AlertState(BatteryOptions())
        self.assertEqual(
            alerts.observe(BatteryReading(9.0, True, "discharging")),
            [AlertKind.persistent],
        )

    def test_cycle_state_round_trips_and_corruption_is_recoverable(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "battery-cycle.json"
            store = CycleStateStore(path)
            state = AlertState(BatteryOptions())
            state.observe(BatteryReading(9.0, True, "discharging"))
            store.save(state)

            restored = store.load(BatteryOptions())
            self.assertEqual(restored.emitted, {20, 10})
            path.write_text("not json", encoding="utf-8")
            self.assertEqual(store.load(BatteryOptions()).emitted, set())

    def test_service_publishes_snapshots_alerts_and_dismissal(self):
        with tempfile.TemporaryDirectory() as directory:
            records = []
            clock = [0.0]
            service = BatteryService(
                records.append,
                Path(directory) / "cycle.json",
                now=lambda: clock[0],
            )
            service.configure({})
            service.update(BatteryReading(55.0, False, "charging", time_to_full=3600))
            service.update(BatteryReading(55.0, True, "discharging", time_to_empty=7200))

            self.assertEqual(records[0]["type"], "data")
            unplugged = next(record for record in records if record["type"] == "publish")
            self.assertEqual(unplugged["context_id"], "battery-unplugged")
            self.assertEqual(unplugged["expires_in_ms"], 3000)
            self.assertEqual(
                unplugged["views"]["compact"]["children"][0]["shape"], "ring"
            )

            service.update(BatteryReading(9.0, True, "discharging", time_to_empty=1800))
            persistent = [record for record in records if record["type"] == "publish"][-1]
            self.assertEqual(persistent["context_id"], "battery-persistent")
            self.assertNotIn("expires_in_ms", persistent)
            self.assertTrue(service.action("dismiss-alert"))
            self.assertEqual(records[-1], {"type": "dismiss", "context_id": "battery-persistent"})

    def test_service_coalesces_estimate_only_updates(self):
        with tempfile.TemporaryDirectory() as directory:
            records = []
            clock = [0.0]
            service = BatteryService(
                records.append,
                Path(directory) / "cycle.json",
                now=lambda: clock[0],
            )
            service.configure({})
            service.update(BatteryReading(60.0, True, "discharging", time_to_empty=7200))
            service.update(BatteryReading(60.0, True, "discharging", time_to_empty=7140))
            self.assertEqual([record["type"] for record in records].count("data"), 1)
            clock[0] = 30.0
            service.update(BatteryReading(60.0, True, "discharging", time_to_empty=7080))
            self.assertEqual([record["type"] for record in records].count("data"), 2)

    def test_pending_upower_reading_waits_until_protocol_ready(self):
        with tempfile.TemporaryDirectory() as directory:
            records = []
            service = BatteryService(records.append, Path(directory) / "cycle.json")
            service.update(BatteryReading(80.0, True, "discharging", time_to_empty=7200))
            service.configure({})
            self.assertEqual(records, [])

            service.ready()
            self.assertEqual(records[0]["type"], "data")


class BatteryProcessTests(unittest.TestCase):
    def test_process_negotiates_and_stops_when_upower_is_unavailable(self):
        environment = os.environ.copy()
        environment["DBUS_SYSTEM_BUS_ADDRESS"] = "unix:path=/nonexistent"
        process = subprocess.Popen(
            [environment["GISLAND_BATTERY_PATH"]],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=environment,
        )
        self.addCleanup(lambda: process.poll() is None and process.kill())
        init = {
            "type": "init",
            "protocol": {
                "minimum": {"major": 1, "minor": 5},
                "maximum": {"major": 1, "minor": 5},
            },
            "capabilities": [
                "data-snapshots",
                "rich-content",
                "independent-views",
                "ring-progress",
            ],
            "configuration": {},
        }
        process.stdin.write(json.dumps(init) + "\n")
        process.stdin.flush()

        readable, _, _ = select.select([process.stdout], [], [], 3.0)
        self.assertTrue(readable)
        ready = json.loads(process.stdout.readline())
        self.assertEqual(ready["type"], "ready")
        self.assertEqual(ready["protocol_minor"], 5)

        process.stdin.write('{"type":"shutdown"}\n')
        process.stdin.flush()
        self.assertEqual(process.wait(timeout=3.0), 0)
        self.assertIn("UPower unavailable", process.stderr.read())


if __name__ == "__main__":
    unittest.main()
