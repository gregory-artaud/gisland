import json
import os
import selectors
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path


def init_message():
    return {
        "type": "init",
        "protocol": {
            "minimum": {"major": 1, "minor": 4},
            "maximum": {"major": 1, "minor": 4},
        },
        "instance_id": "notifications",
        "capabilities": ["context-images", "rich-content", "independent-views"],
        "configuration": {"reveal_duration_ms": 2500},
        "locale": "C",
        "timezone": "UTC",
    }


class NotificationDBusProcessTests(unittest.TestCase):
    def setUp(self):
        if "DBUS_SESSION_BUS_ADDRESS" not in os.environ:
            self.skipTest("requires dbus-run-session")
        root = Path(__file__).resolve().parents[2]
        self.module_root = root / "modules/notifications"
        self.temporary = tempfile.TemporaryDirectory()
        self.environment = dict(os.environ)
        self.environment["PYTHONPATH"] = str(self.module_root)
        self.environment["XDG_STATE_HOME"] = self.temporary.name
        self.start_process()

    def start_process(self):
        self.process = subprocess.Popen(
            [sys.executable, str(self.module_root / "gisland-notifications")],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=self.environment,
            text=True,
        )
        self.send(init_message())

    def stop_process(self):
        if self.process.poll() is None:
            self.send({"type": "shutdown", "deadline_ms": 1000})
            self.process.wait(timeout=3)
        for stream in (self.process.stdin, self.process.stdout, self.process.stderr):
            stream.close()

    def tearDown(self):
        if self.process.poll() is None:
            try:
                self.send({"type": "shutdown", "deadline_ms": 1000})
                self.process.wait(timeout=3)
            except (BrokenPipeError, subprocess.TimeoutExpired):
                self.process.kill()
                self.process.wait(timeout=3)
        if self.process.returncode not in (0, None):
            stderr = self.process.stderr.read()
            if stderr:
                print(stderr, file=sys.stderr)
        for stream in (self.process.stdin, self.process.stdout, self.process.stderr):
            stream.close()
        self.temporary.cleanup()

    def send(self, message):
        self.process.stdin.write(json.dumps(message) + "\n")
        self.process.stdin.flush()

    def record(self, timeout=3):
        selector = selectors.DefaultSelector()
        selector.register(self.process.stdout, selectors.EVENT_READ)
        ready = selector.select(timeout)
        selector.close()
        if not ready:
            stderr = self.process.stderr.read() if self.process.poll() is not None else ""
            self.fail(f"timed out waiting for daemon output: {stderr}")
        line = self.process.stdout.readline()
        if not line:
            self.fail(f"daemon exited early: {self.process.stderr.read()}")
        return json.loads(line)

    def proxy(self):
        import gi

        gi.require_version("Gio", "2.0")
        from gi.repository import Gio

        return Gio.DBusProxy.new_for_bus_sync(
            Gio.BusType.SESSION,
            Gio.DBusProxyFlags.NONE,
            None,
            "org.freedesktop.Notifications",
            "/org/freedesktop/Notifications",
            "org.freedesktop.Notifications",
            None,
        )

    def history_proxy(self):
        import gi

        gi.require_version("Gio", "2.0")
        from gi.repository import Gio

        return Gio.DBusProxy.new_for_bus_sync(
            Gio.BusType.SESSION,
            Gio.DBusProxyFlags.NONE,
            None,
            "org.freedesktop.Notifications",
            "/org/gisland/Notifications/History",
            "org.gisland.Notifications.History",
            None,
        )

    def wait_for_signal(self, signals, expected):
        from gi.repository import GLib

        deadline = time.monotonic() + 2
        context = GLib.MainContext.default()
        while expected not in signals and time.monotonic() < deadline:
            context.iteration(False)
            time.sleep(0.01)
        self.assertIn(expected, signals)

    def test_exports_standard_interface_and_publishes_notifications(self):
        ready = self.record()
        self.assertEqual(ready["type"], "ready")

        import gi

        gi.require_version("Gio", "2.0")
        from gi.repository import Gio, GLib

        proxy = self.proxy()
        signals = []
        proxy.connect(
            "g-signal",
            lambda _proxy, _sender, name, parameters: signals.append((name, parameters.unpack())),
        )
        information = proxy.call_sync(
            "GetServerInformation", None, Gio.DBusCallFlags.NONE, 2000, None
        ).unpack()
        self.assertEqual(information[0], "gisland")
        self.assertEqual(information[2], "development")
        self.assertEqual(information[3], "1.2")

        result = proxy.call_sync(
            "Notify",
            GLib.Variant(
                "(susssasa{sv}i)",
                (
                    "Files",
                    0,
                    "",
                    "Download complete",
                    "The archive is ready",
                    ["default", "Open"],
                    {"urgency": GLib.Variant("y", 1)},
                    1000,
                ),
            ),
            Gio.DBusCallFlags.NONE,
            2000,
            None,
        )
        notification_id = result.unpack()[0]
        publication = self.record()
        self.assertEqual(publication["type"], "publish")
        self.assertEqual(publication["context_id"], f"notification-{notification_id}")
        self.assertEqual(publication["priority"], 20)
        self.assertEqual(set(publication["views"]), {"expanded"})
        self.assertEqual(
            publication["presentation"], {"reveal": "expanded", "duration_ms": 2500}
        )

        proxy.call_sync(
            "CloseNotification",
            GLib.Variant("(u)", (notification_id,)),
            Gio.DBusCallFlags.NONE,
            2000,
            None,
        )
        self.assertEqual(
            self.record(),
            {"type": "dismiss", "context_id": f"notification-{notification_id}"},
        )
        self.wait_for_signal(signals, ("NotificationClosed", (notification_id, 3)))

    def test_replaces_expires_and_emits_named_action(self):
        self.assertEqual(self.record()["type"], "ready")

        import gi

        gi.require_version("Gio", "2.0")
        gi.require_version("GLib", "2.0")
        from gi.repository import Gio, GLib

        proxy = self.proxy()
        signals = []
        proxy.connect(
            "g-signal",
            lambda _proxy, _sender, name, parameters: signals.append((name, parameters.unpack())),
        )

        def notify(replaces_id, actions, timeout):
            return proxy.call_sync(
                "Notify",
                GLib.Variant(
                    "(susssasa{sv}i)",
                    (
                        "Files",
                        replaces_id,
                        "",
                        "Download complete",
                        "The archive is ready",
                        actions,
                        {"urgency": GLib.Variant("y", 1)},
                        timeout,
                    ),
                ),
                Gio.DBusCallFlags.NONE,
                2000,
                None,
            ).unpack()[0]

        notification_id = notify(0, ["show", "Show"], -1)
        self.assertEqual(self.record()["type"], "publish")
        self.assertEqual(notify(notification_id, ["show", "Show"], -1), notification_id)
        self.assertEqual(self.record()["context_id"], f"notification-{notification_id}")

        self.send({"type": "action", "action_id": f"notification-{notification_id}:action-0"})
        self.assertEqual(
            self.record(),
            {"type": "dismiss", "context_id": f"notification-{notification_id}"},
        )
        self.assertEqual(self.record()["type"], "action_result")
        self.wait_for_signal(signals, ("ActionInvoked", (notification_id, "show")))
        self.wait_for_signal(signals, ("NotificationClosed", (notification_id, 2)))

        expiring_id = notify(0, [], 50)
        self.assertEqual(self.record()["context_id"], f"notification-{expiring_id}")
        self.assertEqual(
            self.record(), {"type": "dismiss", "context_id": f"notification-{expiring_id}"}
        )
        self.wait_for_signal(signals, ("NotificationClosed", (expiring_id, 1)))

    def test_private_history_interface_publishes_empty_state(self):
        self.assertEqual(self.record()["type"], "ready")

        import gi

        gi.require_version("Gio", "2.0")
        from gi.repository import Gio

        count = self.history_proxy().call_sync(
            "ShowMore", None, Gio.DBusCallFlags.NONE, 2000, None
        ).unpack()[0]

        self.assertEqual(count, 1)
        publication = self.record()
        self.assertEqual(publication["context_id"], "history")
        self.assertEqual(publication["priority"], 100)
        self.assertNotIn("presentation", publication)

    def test_history_survives_daemon_restart(self):
        self.assertEqual(self.record()["type"], "ready")

        import gi

        gi.require_version("Gio", "2.0")
        from gi.repository import Gio, GLib

        self.proxy().call_sync(
            "Notify",
            GLib.Variant(
                "(susssasa{sv}i)",
                ("Files", 0, "", "Persisted", "Across restart", [], {}, 1000),
            ),
            Gio.DBusCallFlags.NONE,
            2000,
            None,
        )
        self.assertEqual(self.record()["type"], "publish")
        self.stop_process()
        self.start_process()
        self.assertEqual(self.record()["type"], "ready")

        self.assertEqual(
            self.history_proxy()
            .call_sync("ShowMore", None, Gio.DBusCallFlags.NONE, 2000, None)
            .unpack()[0],
            1,
        )
        publication = self.record()

        def text_values(node):
            values = [node["value"]] if node.get("type") == "text" else []
            for child in node.get("children", []):
                values.extend(text_values(child))
            if isinstance(node.get("content"), dict):
                values.extend(text_values(node["content"]))
            return values

        self.assertTrue(
            any(
                value.startswith("Persisted")
                for value in text_values(publication["views"]["expanded"])
            )
        )

    def test_rejects_a_second_bus_name_owner(self):
        self.assertEqual(self.record()["type"], "ready")
        root = Path(__file__).resolve().parents[2]
        module_root = root / "modules/notifications"
        environment = dict(os.environ)
        environment["PYTHONPATH"] = str(module_root)
        second = subprocess.Popen(
            [sys.executable, str(module_root / "gisland-notifications")],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=environment,
            text=True,
        )
        try:
            second.stdin.write(json.dumps(init_message()) + "\n")
            second.stdin.flush()
            second.wait(timeout=3)
            self.assertEqual(second.stdout.read(), "")
            self.assertIn("could not own", second.stderr.read())
            self.assertEqual(second.returncode, 1)
        finally:
            if second.poll() is None:
                second.kill()
                second.wait(timeout=3)
            for stream in (second.stdin, second.stdout, second.stderr):
                stream.close()


if __name__ == "__main__":
    unittest.main()
