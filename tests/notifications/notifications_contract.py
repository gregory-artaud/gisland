#!/usr/bin/env python3

import argparse
from collections import deque
import ctypes
import json
import os
import queue
import stat
import shutil
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


BUS_NAME = "org.freedesktop.Notifications"
OBJECT_PATH = "/org/freedesktop/Notifications"
INTERFACE = BUS_NAME
CAPABILITIES = [
    "actions",
    "body",
    "body-markup",
    "body-hyperlinks",
    "body-images",
    "icon-static",
    "persistence",
]
PROTOCOL_CAPABILITIES = ["context-images", "rich-content", "independent-views"]
MAX_LIVE_NOTIFICATIONS = 128


def parse_command(value):
    command = json.loads(value)
    if not isinstance(command, list) or not command:
        raise argparse.ArgumentTypeError("command must be a non-empty JSON array")
    return command


def init_message(protocol_minor, configuration=None):
    return {
        "type": "init",
        "protocol": {
            "minimum": {"major": 1, "minor": protocol_minor},
            "maximum": {"major": 1, "minor": protocol_minor},
        },
        "instance_id": "notifications",
        "capabilities": PROTOCOL_CAPABILITIES,
        "configuration": configuration or {},
        "locale": "C",
        "timezone": "UTC",
    }


class ModuleProcess:
    def __init__(self, command, environment, protocol_minor, configuration=None):
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
        self.send(init_message(protocol_minor, configuration))
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
                f"timed out waiting for output; exit={self.process.poll()}; stderr={diagnostics!r}"
            ) from error

    def collect(self, quiet=0.1):
        result = []
        while True:
            try:
                result.append(self.records.get(timeout=quiet))
            except queue.Empty:
                return result

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
                if NotificationContract.protocol_minor >= 8:
                    if record.get("invocation_id") != invocation:
                        raise AssertionError(f"wrong invocation ID: {record!r}")
                return records, record
            records.append(record)

    def close(self):
        if self.process.poll() is None:
            try:
                self.send(
                    {"type": "shutdown", "reason": "test-complete", "deadline_ms": 1000}
                )
                self.process.wait(timeout=2.0)
            except (BrokenPipeError, subprocess.TimeoutExpired):
                self.process.kill()
                self.process.wait(timeout=2.0)


class NotificationContract(unittest.TestCase):
    command = None
    protocol_minor = None
    legacy_history_dbus = False

    def setUp(self):
        if "DBUS_SESSION_BUS_ADDRESS" not in os.environ:
            self.skipTest("requires dbus-run-session")
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.environment = os.environ.copy()
        self.environment["XDG_STATE_HOME"] = str(self.root / "state")
        self.module = None

    def tearDown(self):
        if self.module is not None:
            self.module.close()
        self.temporary.cleanup()

    def start(self, configuration=None):
        self.module = ModuleProcess(
            self.command, self.environment, self.protocol_minor, configuration
        )
        self.assertEqual(
            self.module.ready,
            {
                "type": "ready",
                "protocol_major": 1,
                "protocol_minor": self.protocol_minor,
                "capabilities": PROTOCOL_CAPABILITIES,
            },
        )
        return self.module

    @staticmethod
    def proxy():
        return Gio.DBusProxy.new_for_bus_sync(
            Gio.BusType.SESSION,
            Gio.DBusProxyFlags.NONE,
            None,
            BUS_NAME,
            OBJECT_PATH,
            INTERFACE,
            None,
        )

    @staticmethod
    def history_proxy():
        return Gio.DBusProxy.new_for_bus_sync(
            Gio.BusType.SESSION,
            Gio.DBusProxyFlags.NONE,
            None,
            BUS_NAME,
            "/org/gisland/Notifications/History",
            "org.gisland.Notifications.History",
            None,
        )

    @staticmethod
    def call(proxy, method, parameters=None):
        result = proxy.call_sync(method, parameters, Gio.DBusCallFlags.NONE, 10000, None)
        return None if result is None else result.unpack()

    @staticmethod
    def remote_error(error):
        return Gio.DBusError.get_remote_error(error)

    def wait_history(self, expected_summary=None):
        path = self.root / "state/gisland/notifications-history.json"
        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline:
            try:
                document = json.loads(path.read_text(encoding="utf-8"))
                if expected_summary is None or (
                    document["records"] and document["records"][0]["summary"] == expected_summary
                ):
                    return document
            except (FileNotFoundError, json.JSONDecodeError, KeyError):
                pass
            time.sleep(0.01)
        self.fail(f"history did not reach expected state: {expected_summary!r}")

    @staticmethod
    def notify(proxy, *, app="Files", replaces=0, icon="", summary="Ready", body="",
               actions=(), hints=None, timeout=0):
        return NotificationContract.call(
            proxy,
            "Notify",
            GLib.Variant(
                "(susssasa{sv}i)",
                (app, replaces, icon, summary, body, list(actions), hints or {}, timeout),
            ),
        )[0]

    @staticmethod
    def text_values(node):
        values = [node["value"]] if node.get("type") == "text" else []
        if node.get("type") == "link":
            values.append(node["value"])
        for child in node.get("children", []):
            values.extend(NotificationContract.text_values(child))
        if isinstance(node.get("content"), dict):
            values.extend(NotificationContract.text_values(node["content"]))
        elif isinstance(node.get("content"), list):
            for child in node["content"]:
                values.extend(NotificationContract.text_values(child))
        return values

    @staticmethod
    def wait_signal(signals, expected):
        context = GLib.MainContext.default()
        deadline = time.monotonic() + 2.0
        while expected not in signals and time.monotonic() < deadline:
            context.iteration(False)
            time.sleep(0.005)
        if expected not in signals:
            raise AssertionError(f"missing signal {expected!r}; got {signals!r}")

    def test_public_information_and_complete_notify_shape(self):
        module = self.start({"reveal_duration_ms": 2500})
        proxy = self.proxy()
        self.assertEqual(self.call(proxy, "GetCapabilities"), (CAPABILITIES,))
        information = self.call(proxy, "GetServerInformation")
        self.assertEqual((information[0], information[1], information[3]), ("gisland", "gisland", "1.2"))

        image = (2, 1, 8, True, 8, 4, bytes((255, 0, 0, 128, 0, 255, 0, 255)))
        notification_id = self.notify(
            proxy,
            app="Files",
            summary="Download complete",
            body='Before <b>bold</b> <a href="https://example.com">open</a>',
            actions=("default", "Open", "show", "Show folder"),
            hints={
                "urgency": GLib.Variant("y", 2),
                "resident": GLib.Variant("b", True),
                "transient": GLib.Variant("b", True),
                "image-data": GLib.Variant("(iiibiiay)", image),
                "unknown": GLib.Variant("s", "ignored"),
            },
            timeout=-1,
        )
        publication = module.next_record()
        self.assertEqual(publication.get("type"), "publish", publication)
        self.assertEqual(notification_id, 1)
        self.assertEqual(publication["context_id"], "notification-1")
        self.assertEqual(publication["priority"], 30)
        self.assertEqual(publication["presentation"], {"reveal": "expanded", "duration_ms": 2500})
        self.assertEqual(publication["resources"][0], {
            "id": "app-image", "format": "rgba8", "width": 2, "height": 1,
            "data": "/wAAgAD/AP8=",
        })
        expanded = publication["views"]["expanded"]
        self.assertEqual(expanded["type"], "action_region")
        self.assertEqual(expanded["action_id"], "notification-1:default")
        self.assertIn("Download complete", self.text_values(expanded))
        self.assertIn("bold", self.text_values(expanded))

    def test_replacement_ids_timeout_residency_actions_and_close_reasons(self):
        module = self.start()
        proxy = self.proxy()
        signals = []
        proxy.connect(
            "g-signal",
            lambda _proxy, _sender, name, parameters: signals.append((name, parameters.unpack())),
        )
        first = self.notify(proxy, actions=("show", "Show"), timeout=80)
        self.assertEqual(module.next_record()["context_id"], "notification-1")
        self.assertEqual(self.notify(proxy, replaces=first, summary="Updated", actions=("show", "Show"), timeout=0), first)
        self.assertEqual(module.next_record()["context_id"], "notification-1")
        second = self.notify(proxy, actions=("default", "Open"), hints={"resident": GLib.Variant("b", True)}, timeout=0)
        self.assertEqual(second, 2)
        module.next_record()

        records, result = module.action("notification-1:action-0")
        self.assertEqual(records, [{"type": "dismiss", "context_id": "notification-1"}])
        self.assertTrue(result["accepted"])
        self.wait_signal(signals, ("ActionInvoked", (1, "show")))
        self.wait_signal(signals, ("NotificationClosed", (1, 2)))

        records, result = module.action("notification-2:default")
        self.assertEqual(records, [])
        self.assertTrue(result["accepted"])
        self.wait_signal(signals, ("ActionInvoked", (2, "default")))
        self.call(proxy, "CloseNotification", GLib.Variant("(u)", (2,)))
        self.assertEqual(module.next_record(), {"type": "dismiss", "context_id": "notification-2"})
        self.wait_signal(signals, ("NotificationClosed", (2, 3)))
        nested = self.notify(proxy, actions=("default", "Open"), timeout=0)
        module.next_record()
        records, result = module.action(f"notification-{nested}:close")
        self.assertEqual(records, [{"type": "dismiss", "context_id": f"notification-{nested}"}])
        self.assertTrue(result["accepted"])
        self.wait_signal(signals, ("NotificationClosed", (nested, 2)))
        self.assertFalse(
            any(name == "ActionInvoked" and values[0] == nested for name, values in signals)
        )
        with self.assertRaises(GLib.Error) as caught:
            self.call(proxy, "CloseNotification", GLib.Variant("(u)", (999,)))
        self.assertEqual(
            self.remote_error(caught.exception),
            "org.freedesktop.Notifications.Error.NonExistent",
        )
        self.assertEqual(str(caught.exception).rsplit(":", 1)[-1].strip(), "(36)")
        self.assertFalse(any(name == "NotificationClosed" and values[0] == 999 for name, values in signals))

        expiring = self.notify(proxy, timeout=30)
        module.next_record()
        self.assertEqual(module.next_record(), {"type": "dismiss", "context_id": f"notification-{expiring}"})
        self.wait_signal(signals, ("NotificationClosed", (expiring, 1)))

    def test_unknown_replacement_ids_allocate_fresh_ids_and_failed_replacement_is_atomic(self):
        module = self.start()
        proxy = self.proxy()
        maximum = self.notify(proxy, replaces=0xFFFFFFFF, summary="Maximum", timeout=0)
        self.assertEqual(maximum, 1)
        self.assertEqual(module.next_record()["context_id"], "notification-1")
        unknown = self.notify(proxy, replaces=77, summary="Unknown", timeout=0)
        self.assertEqual(unknown, 2)
        module.next_record()
        self.assertEqual(
            self.notify(proxy, replaces=maximum, summary="Replaced", timeout=0),
            maximum,
        )
        self.assertEqual(module.next_record()["context_id"], "notification-1")
        allocated = self.notify(proxy, summary="Allocated", timeout=0)
        self.assertEqual(allocated, 3)
        module.next_record()

        with self.assertRaises(GLib.Error) as caught:
            self.notify(proxy, replaces=maximum, summary="x" * 4097, timeout=0)
        self.assertEqual(self.remote_error(caught.exception), "org.freedesktop.DBus.Error.InvalidArgs")
        records, result = module.action("notification-1:close")
        self.assertEqual(records, [{"type": "dismiss", "context_id": "notification-1"}])
        self.assertTrue(result["accepted"])

        stale = self.notify(proxy, replaces=maximum, summary="Stale", timeout=0)
        self.assertEqual(stale, 4)
        self.assertEqual(module.next_record()["context_id"], "notification-4")

        timed = self.notify(proxy, summary="Old timer", timeout=150)
        module.next_record()
        with self.assertRaises(GLib.Error):
            self.notify(proxy, replaces=timed, summary="x" * 4097, timeout=0)
        self.assertEqual(
            module.next_record(timeout=0.4),
            {"type": "dismiss", "context_id": f"notification-{timed}"},
        )

    def test_replacement_timer_is_rearmed_past_the_old_expiry(self):
        module = self.start()
        proxy = self.proxy()
        notification_id = self.notify(proxy, timeout=80)
        module.next_record()
        time.sleep(0.04)
        self.assertEqual(self.notify(proxy, replaces=notification_id, timeout=250), notification_id)
        module.next_record()
        time.sleep(0.08)
        self.assertEqual(module.collect(quiet=0.03), [])
        self.assertEqual(
            module.next_record(timeout=0.3),
            {"type": "dismiss", "context_id": f"notification-{notification_id}"},
        )

    def test_zero_timeout_notifications_evict_the_oldest_live_context_at_capacity(self):
        module = self.start({"history_limit": 1, "history_visible_limit": 1})
        proxy = self.proxy()
        signals = []
        proxy.connect(
            "g-signal",
            lambda _proxy, _sender, name, parameters: signals.append((name, parameters.unpack())),
        )
        for index in range(MAX_LIVE_NOTIFICATIONS):
            self.assertEqual(self.notify(proxy, summary=f"Live {index}", timeout=0), index + 1)
            self.assertEqual(module.next_record()["context_id"], f"notification-{index + 1}")

        newest = self.notify(proxy, summary="Capacity replacement", timeout=0)
        self.assertEqual(newest, MAX_LIVE_NOTIFICATIONS + 1)
        self.assertEqual(
            module.next_record(),
            {"type": "dismiss", "context_id": "notification-1"},
        )
        self.assertEqual(module.next_record()["context_id"], f"notification-{newest}")
        self.wait_signal(signals, ("NotificationClosed", (1, 4)))
        records, result = module.action("notification-1:close")
        self.assertEqual(records, [])
        self.assertFalse(result["accepted"])
        with self.assertRaises(GLib.Error) as caught:
            self.call(proxy, "CloseNotification", GLib.Variant("(u)", (1,)))
        self.assertEqual(
            self.remote_error(caught.exception),
            "org.freedesktop.Notifications.Error.NonExistent",
        )

        following = self.notify(proxy, summary="Next replacement", timeout=0)
        self.assertEqual(
            module.next_record(),
            {"type": "dismiss", "context_id": "notification-2"},
        )
        self.assertEqual(module.next_record()["context_id"], f"notification-{following}")
        self.wait_signal(signals, ("NotificationClosed", (2, 4)))

    def test_transient_history_replacement_and_restart_semantics(self):
        module = self.start()
        proxy = self.proxy()
        transient = self.notify(
            proxy,
            replaces=66,
            summary="Never stored",
            hints={"transient": GLib.Variant("b", True)},
        )
        self.assertEqual(transient, 1)
        module.next_record()
        time.sleep(0.1)
        self.assertFalse((self.root / "state/gisland/notifications-history.json").exists())
        persistent = self.notify(proxy, replaces=77, summary="Persistent")
        self.assertEqual(persistent, 2)
        module.next_record()
        self.assertEqual(
            self.wait_history("Persistent")["records"][0]["notification_id"],
            persistent,
        )

        self.assertEqual(
            self.notify(
                proxy,
                replaces=persistent,
                summary="Transient",
                hints={"transient": GLib.Variant("b", True)},
            ),
            persistent,
        )
        module.next_record()
        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline:
            if self.wait_history()["records"] == []:
                break
            time.sleep(0.01)
        else:
            self.fail("persistent replacement was not removed from history")

        self.assertEqual(
            self.notify(proxy, replaces=persistent, summary="Persistent again"),
            persistent,
        )
        module.next_record()
        document = self.wait_history("Persistent again")
        sequence = document["records"][0]["sequence"]
        module.close()
        self.module = None
        module = self.start()
        proxy = self.proxy()
        restarted_id = self.notify(proxy, replaces=persistent, summary="After restart")
        self.assertEqual(restarted_id, 1)
        module.next_record()
        restarted = self.wait_history("After restart")
        self.assertGreater(restarted["records"][0]["sequence"], sequence)

    def test_markup_inline_raster_svg_and_remote_rejection(self):
        module = self.start()
        proxy = self.proxy()
        baseline = Path(__file__).resolve().parents[1] / "baselines/notification-compact.png"
        svg = self.root / "icon.svg"
        svg.write_text(
            '<svg xmlns="http://www.w3.org/2000/svg" width="2" height="1">'
            '<rect width="2" height="1" fill="#ff0000"/></svg>',
            encoding="utf-8",
        )
        notification_id = self.notify(
            proxy,
            icon=str(baseline),
            body=(
                'plain <i>italic <u>under</u></i> '
                f'<img src="{baseline.as_uri()}" alt="Preview"/> '
                '<img src="https://example.com/no.png" alt="Remote"/>'
            ),
        )
        publication = module.next_record()
        self.assertEqual(notification_id, 1)
        self.assertEqual([resource["id"] for resource in publication["resources"]], ["app-image", "inline-0"])
        items = publication["views"]["expanded"]["children"][1]["content"]
        self.assertTrue(any(item.get("emphasis") == ["italic", "underline"] for item in items))
        self.assertTrue(any(item.get("resource_id") == "inline-0" for item in items))
        self.assertFalse(any(item.get("accessible_label") == "Remote" for item in items))

        self.notify(proxy, icon=str(svg), summary="SVG")
        svg_publication = module.next_record()
        self.assertEqual(
            (svg_publication["resources"][0]["width"], svg_publication["resources"][0]["height"]),
            (2, 1),
        )

        malformed = "Keep <b>all"
        self.notify(proxy, body=malformed)
        publication = module.next_record()
        self.assertIn(malformed, self.text_values(publication["views"]["expanded"]))

    def test_image_data_bounds_stride_and_rgb_channels(self):
        module = self.start()
        proxy = self.proxy()
        valid_rgb = (2, 1, 8, False, 8, 3, bytes((1, 2, 3, 4, 5, 6, 0, 0)))
        self.notify(proxy, hints={"image-data": GLib.Variant("(iiibiiay)", valid_rgb)})
        publication = module.next_record()
        self.assertEqual(publication.get("type"), "publish", publication)
        resource = publication["resources"][0]
        self.assertEqual(resource, {
            "id": "app-image", "format": "rgba8", "width": 2, "height": 1,
            "data": "AQID/wQFBv8=",
        })
        invalid = (1, 1, 3, True, 8, 4, bytes((1, 2, 3, 4)))
        self.notify(proxy, hints={"image-data": GLib.Variant("(iiibiiay)", invalid)})
        self.assertNotIn("resources", module.next_record())

    def test_wrong_and_oversized_image_hints_are_ignored_before_expansion(self):
        module = self.start()
        proxy = self.proxy()
        oversized = (1025, 1024, 4100, True, 8, 4, bytes(4100 * 1024))
        self.notify(
            proxy,
            hints={
                "image-data": GLib.Variant("(iiibiiay)", oversized),
                "image-path": GLib.Variant("u", 7),
            },
        )
        publication = module.next_record()
        self.assertNotIn("resources", publication)

        self.notify(
            proxy,
            hints={
                "image-data": GLib.Variant("s", "not-an-image"),
                "image_path": GLib.Variant("b", True),
            },
        )
        self.assertNotIn("resources", module.next_record())
        baseline = Path(__file__).resolve().parents[1] / "baselines/notification-compact.png"
        self.notify(
            proxy,
            hints={
                "image-data": GLib.Variant("s", "not-an-image"),
                "image-path": GLib.Variant("s", str(baseline)),
            },
        )
        self.assertEqual(module.next_record()["resources"][0]["id"], "app-image")

    def test_file_images_require_strict_local_regular_bounded_sources(self):
        module = self.start()
        proxy = self.proxy()
        baseline = Path(__file__).resolve().parents[1] / "baselines/notification-compact.png"
        symlink = self.root / "linked.png"
        symlink.symlink_to(baseline)
        oversized_svg = self.root / "oversized.svg"
        with oversized_svg.open("wb") as output:
            output.write(b'<svg xmlns="http://www.w3.org/2000/svg">')
            output.write(b" " * (4 * 1024 * 1024))
            output.write(b"</svg>")
        complex_svg = self.root / "complex.svg"
        complex_svg.write_text(
            '<svg xmlns="http://www.w3.org/2000/svg" width="2" height="2">'
            + '<g/>' * 4097
            + "</svg>",
            encoding="utf-8",
        )
        bomb = self.root / "bomb.png"
        payload = bytearray(baseline.read_bytes())
        payload[16:24] = (1_000_001).to_bytes(4, "big") + (1_000_001).to_bytes(4, "big")
        bomb.write_bytes(payload)

        invalid_sources = (
            str(symlink),
            oversized_svg.as_uri(),
            str(complex_svg),
            str(bomb),
            f"file://localhostevil{baseline}",
            baseline.as_uri() + "?query=1",
            baseline.as_uri() + "#fragment",
            "file://remotehost" + str(baseline),
        )
        for source in invalid_sources:
            self.notify(proxy, icon=source)
            self.assertNotIn("resources", module.next_record(), source)

        self.notify(proxy, icon="file://localhost" + str(baseline))
        self.assertEqual(module.next_record()["resources"][0]["id"], "app-image")

    def test_file_image_is_decoded_from_the_single_checked_read(self):
        module = self.start()
        proxy = self.proxy()
        baseline = Path(__file__).resolve().parents[1] / "baselines/notification-compact.png"
        source = self.root / "changing-image"
        replacement = self.root / "replacement.svg"
        source.write_bytes(baseline.read_bytes())
        replacement.write_text(
            '<svg xmlns="http://www.w3.org/2000/svg" width="2" height="1">'
            '<rect width="2" height="1" fill="#00ff00"/></svg>',
            encoding="utf-8",
        )

        libc = ctypes.CDLL(None, use_errno=True)
        inotify_fd = libc.inotify_init1(os.O_CLOEXEC)
        self.assertGreaterEqual(inotify_fd, 0, os.strerror(ctypes.get_errno()))
        watch = libc.inotify_add_watch(inotify_fd, os.fsencode(source), 0x00000010)
        self.assertGreaterEqual(watch, 0, os.strerror(ctypes.get_errno()))
        swapped = threading.Event()

        def replace_after_first_close():
            try:
                os.read(inotify_fd, 4096)
                os.replace(replacement, source)
                swapped.set()
            finally:
                os.close(inotify_fd)

        worker = threading.Thread(target=replace_after_first_close, daemon=True)
        worker.start()
        self.notify(proxy, icon=str(source))
        publication = module.next_record()
        worker.join(timeout=2.0)
        self.assertTrue(swapped.is_set(), "image loader did not close its checked source")
        resource = publication["resources"][0]
        self.assertEqual((resource["width"], resource["height"]), (480, 360))

    def test_desktop_entries_use_a_bounded_nonblocking_regular_file_read(self):
        data_home = self.root / "data"
        applications = data_home / "applications"
        applications.mkdir(parents=True)
        baseline = Path(__file__).resolve().parents[1] / "baselines/notification-compact.png"
        entry = applications / "safe.desktop"
        entry.write_text(
            f"[Desktop Entry]\nType=Application\nIcon={baseline}\n",
            encoding="utf-8",
        )
        self.environment["XDG_DATA_HOME"] = str(data_home)
        module = self.start()
        proxy = self.proxy()

        self.notify(
            proxy,
            hints={"desktop-entry": GLib.Variant("s", "safe")},
        )
        self.assertEqual(module.next_record()["resources"][0]["id"], "app-image")

        entry.unlink()
        os.mkfifo(entry)
        started = time.monotonic()
        self.notify(
            proxy,
            hints={"desktop-entry": GLib.Variant("s", "safe")},
        )
        self.assertNotIn("resources", module.next_record())
        self.assertLess(time.monotonic() - started, 1.0)

    def test_history_close_uses_lua_host_bindir_for_gislandctl(self):
        control_dir = self.root / "host-bin"
        control_dir.mkdir()
        host = control_dir / "gisland-lua-host"
        shutil.copy2(self.command[0], host)
        marker = self.root / "gislandctl-command"
        control = control_dir / "gislandctl"
        control.write_text(
            "#!/bin/sh\nprintf '%s\\n' \"$*\" >\"$GISLAND_TEST_CONTROL_MARKER\"\n",
            encoding="utf-8",
        )
        control.chmod(0o755)
        self.environment["GISLAND_TEST_CONTROL_MARKER"] = str(marker)
        self.environment["PATH"] = ""
        command = [str(host), *self.command[1:]]
        self.module = ModuleProcess(command, self.environment, self.protocol_minor)
        module = self.module
        self._show_more()
        publication = module.next_record()
        session = publication["views"]["expanded"]["children"][0]["children"][2][
            "action_id"
        ].split(":")[1]
        records, result = module.action(f"history:{session}:close-all")
        self.assertEqual(records, [{"type": "dismiss", "context_id": "history"}])
        self.assertTrue(result["accepted"])
        deadline = time.monotonic() + 2.0
        while not marker.exists() and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertEqual(marker.read_text(encoding="utf-8"), "close\n")

    def test_body_action_markup_and_resource_protocol_budgets(self):
        module = self.start()
        proxy = self.proxy()
        body = "é" * 10000
        self.notify(proxy, body=body)
        publication = module.next_record()
        spans = publication["views"]["expanded"]["children"][1]["content"]
        self.assertTrue(all(len(item.get("value", "").encode()) <= 4096 for item in spans))
        self.assertEqual(sum(len(item.get("value", "").encode()) for item in spans), 16 * 1024)

        deep = "<b>" * 20 + "safe" + "</b>" * 20
        self.notify(proxy, body=deep)
        self.assertIn(deep[:4096], self.text_values(module.next_record()["views"]["expanded"]))

        with self.assertRaises(GLib.Error) as caught:
            self.notify(proxy, body="x" * (64 * 1024 + 1))
        self.assertEqual(self.remote_error(caught.exception), "org.freedesktop.DBus.Error.InvalidArgs")
        with self.assertRaises(GLib.Error):
            self.notify(proxy, actions=tuple(value for index in range(33) for value in (str(index), "label")))
        with self.assertRaises(GLib.Error):
            self.notify(proxy, app="x" * 1025)
        with self.assertRaises(GLib.Error):
            self.notify(proxy, actions=("key", "x" * 4097))
        with self.assertRaises(GLib.Error):
            self.notify(
                proxy,
                hints={f"unknown-{index}": GLib.Variant("u", index) for index in range(65)},
            )

        baseline = Path(__file__).resolve().parents[1] / "baselines/notification-compact.png"
        images = "".join(f'<img src="{baseline}" alt="{index}"/>' for index in range(20))
        self.notify(proxy, icon=str(baseline), body=images)
        resources = module.next_record()["resources"]
        self.assertLessEqual(len(resources), 16)
        self.assertEqual(resources[0]["id"], "app-image")
        decoded_total = sum(len(GLib.base64_decode(resource["data"])) for resource in resources)
        self.assertLessEqual(decoded_total, 4 * 1024 * 1024)

    def _show_more(self):
        if self.legacy_history_dbus:
            return self.call(self.history_proxy(), "ShowMore")[0]
        records, result = self.module.action("show-more")
        self.assertTrue(result["accepted"])
        self.assertEqual(len(records), 1)
        self.module.pending.extend(records)
        return None

    def test_history_persistence_order_cap_visibility_masks_and_reset(self):
        module = self.start({"history_limit": 100, "history_visible_limit": 5})
        proxy = self.proxy()
        for index in range(105):
            self.notify(proxy, summary=f"Item {index}", body="safe <b>body</b>")
            module.next_record()
        state_path = self.root / "state/gisland/notifications-history.json"
        deadline = time.monotonic() + 2.0
        while not state_path.exists() and time.monotonic() < deadline:
            time.sleep(0.01)
        while True:
            document = json.loads(state_path.read_text(encoding="utf-8"))
            if document["records"][0]["summary"] == "Item 104":
                break
            if time.monotonic() >= deadline:
                self.fail(f"history save did not catch up: {document!r}")
            time.sleep(0.01)
        self.assertEqual(document["version"], 1)
        self.assertEqual(len(document["records"]), 100)
        self.assertEqual(document["records"][0]["summary"], "Item 104")
        self.assertEqual(document["records"][-1]["summary"], "Item 5")
        self.assertEqual(document["records"][0]["body"], "safe body")
        self.assertEqual(stat.S_IMODE(state_path.stat().st_mode), 0o600)
        self.assertEqual(list(state_path.parent.glob("*.tmp")), [])

        heights = []
        for count in range(1, 6):
            returned = self._show_more()
            if returned is not None:
                self.assertEqual(returned, count)
            publication = module.next_record()
            values = self.text_values(publication["views"]["expanded"])
            heights.append(len([value for value in values if value.startswith("Item ")]))
        self.assertEqual(heights, [1, 2, 3, 4, 5])
        module.send({"type": "visibility", "visibility": "expanded-active"})
        session = publication["views"]["expanded"]["children"][1]["action_id"].split(":")[1]
        sequence = publication["views"]["expanded"]["children"][2]["action_id"].split(":")[-1]
        records, result = module.action(f"history:{session}:hide:{sequence}")
        self.assertTrue(result["accepted"])
        self.assertEqual(len(records), 1)
        module.send({"type": "visibility", "visibility": "hidden"})
        self.assertEqual(module.next_record(), {"type": "dismiss", "context_id": "history"})
        self._show_more()
        publication = module.next_record()
        self.assertEqual(
            len([value for value in self.text_values(publication["views"]["expanded"]) if value.startswith("Item ")]),
            1,
        )

    def test_new_notification_while_history_expanded_preserves_count_and_order(self):
        module = self.start()
        proxy = self.proxy()
        for summary in ("First", "Second"):
            self.notify(proxy, summary=summary)
            module.next_record()
        self._show_more()
        module.next_record()
        self._show_more()
        module.next_record()
        module.send({"type": "visibility", "visibility": "expanded-active"})
        self.notify(proxy, summary="Newest")
        live = module.next_record()
        history = module.next_record()
        self.assertNotIn("presentation", live)
        values = self.text_values(history["views"]["expanded"])
        self.assertEqual([value for value in values if value in ("Newest", "Second", "First")], ["Newest", "Second"])

    def test_history_replacement_moves_one_record_to_newest(self):
        module = self.start()
        proxy = self.proxy()
        first = self.notify(proxy, summary="First")
        module.next_record()
        self.notify(proxy, summary="Second")
        module.next_record()
        self.assertEqual(self.notify(proxy, replaces=first, summary="Updated"), first)
        module.next_record()
        self._show_more()
        first_page = module.next_record()
        self._show_more()
        second_page = module.next_record()
        values = self.text_values(second_page["views"]["expanded"])
        self.assertEqual([value for value in values if value in ("Updated", "Second", "First")], ["Updated", "Second"])
        self.assertNotIn("First", values)

    def test_replacing_an_evicted_live_notification_allocates_a_new_history_sequence(self):
        module = self.start({"history_limit": 1, "history_visible_limit": 1})
        proxy = self.proxy()
        first = self.notify(proxy, summary="First")
        module.next_record()
        first_sequence = self.wait_history("First")["records"][0]["sequence"]
        self.notify(proxy, summary="Second")
        module.next_record()
        self.wait_history("Second")
        self.assertEqual(self.notify(proxy, replaces=first, summary="First again"), first)
        module.next_record()
        replacement = self.wait_history("First again")["records"][0]
        self.assertGreater(replacement["sequence"], first_sequence)

    def test_unconfirmed_history_and_confirmed_inactivity_close(self):
        module = self.start()
        self._show_more()
        module.next_record()
        self.assertEqual(
            module.next_record(timeout=3.0),
            {"type": "dismiss", "context_id": "history"},
        )
        self._show_more()
        module.next_record()
        module.send({"type": "visibility", "visibility": "expanded-active"})
        self.assertEqual(
            module.next_record(timeout=9.0),
            {"type": "dismiss", "context_id": "history"},
        )

    def test_show_more_while_expanded_does_not_restore_the_two_second_timer(self):
        module = self.start()
        proxy = self.proxy()
        self.notify(proxy, summary="First")
        module.next_record()
        self.notify(proxy, summary="Second")
        module.next_record()
        self._show_more()
        module.next_record()
        module.send({"type": "visibility", "visibility": "expanded-active"})
        time.sleep(0.1)
        self._show_more()
        module.next_record()
        time.sleep(2.3)
        self.assertEqual(module.collect(quiet=0.05), [])
        self.assertEqual(
            module.next_record(timeout=6.0),
            {"type": "dismiss", "context_id": "history"},
        )

    def test_show_more_uses_latest_expanded_visibility_immediately(self):
        module = self.start()
        module.send({"type": "visibility", "visibility": "expanded-active"})
        self._show_more()
        module.next_record()
        time.sleep(2.3)
        self.assertEqual(module.collect(quiet=0.05), [])

    def test_shutdown_persists_latest_queued_history_without_async_callbacks(self):
        self.environment["GISLAND_NOTIFICATIONS_TEST_WRITE_DELAY_MS"] = "5000"
        module = self.start()
        proxy = self.proxy()
        notification_id = self.notify(proxy, summary="Delayed N", timeout=60)
        module.next_record()
        queued_id = self.notify(proxy, summary="Queued N+1", timeout=0)
        self.assertEqual(queued_id, notification_id + 1)
        module.next_record()
        started = time.monotonic()
        self.assertEqual(self.call(proxy, "GetCapabilities"), (CAPABILITIES,))
        self.assertLess(time.monotonic() - started, 0.5)
        self.assertEqual(
            module.next_record(timeout=0.5),
            {"type": "dismiss", "context_id": f"notification-{notification_id}"},
        )
        started = time.monotonic()
        module.close()
        self.module = None
        self.assertLess(time.monotonic() - started, 1.5)
        state_path = self.root / "state/gisland/notifications-history.json"
        document = json.loads(state_path.read_text(encoding="utf-8"))
        self.assertEqual(
            [record["summary"] for record in document["records"]],
            ["Queued N+1", "Delayed N"],
        )
        self.assertEqual(stat.S_IMODE(state_path.stat().st_mode), 0o600)
        self.assertEqual(list(state_path.parent.iterdir()), [state_path])
        time.sleep(0.05)
        self.assertEqual(module.collect(quiet=0.05), [])
        stderr_lines = []
        while not module.stderr.empty():
            stderr_lines.append(module.stderr.get_nowait())
        self.assertFalse(
            any(line.startswith("gisland-notifications:") for line in stderr_lines),
            "\n".join(stderr_lines),
        )

    def test_history_write_errors_are_logs_only(self):
        module = self.start()
        proxy = self.proxy()
        state_directory = self.root / "state/gisland"
        shutil.rmtree(state_directory)
        state_directory.write_text("not a directory", encoding="utf-8")
        self.assertEqual(self.notify(proxy, summary="Still delivered"), 1)
        self.assertEqual(module.next_record()["context_id"], "notification-1")
        deadline = time.monotonic() + 2.0
        records = []
        while time.monotonic() < deadline:
            try:
                records.append(module.records.get(timeout=0.05))
            except queue.Empty:
                pass
            if any(
                record.get("type") == "log"
                and "could not save notification history" in record.get("message", "")
                for record in records
            ):
                break
        self.assertTrue(
            any(
                record.get("type") == "log"
                and "could not save notification history" in record.get("message", "")
                for record in records
            )
        )
        self.assertEqual(self.call(proxy, "GetCapabilities"), (CAPABILITIES,))

    def test_malformed_calls_and_name_contention_are_isolated(self):
        module = self.start()
        proxy = self.proxy()
        with self.assertRaises(GLib.Error) as caught:
            self.notify(proxy, actions=("odd",), timeout=0)
        self.assertEqual(self.remote_error(caught.exception), "org.freedesktop.DBus.Error.InvalidArgs")
        self.assertEqual(module.collect(), [])

        second = subprocess.Popen(
            self.command,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            env=self.environment,
        )
        try:
            second.stdin.write(json.dumps(init_message(self.protocol_minor)) + "\n")
            second.stdin.flush()
            second.wait(timeout=3.0)
            self.assertNotEqual(second.returncode, 0)
            self.assertEqual(second.stdout.read(), "")
            self.assertIn("could not own org.freedesktop.Notifications", second.stderr.read())
        finally:
            if second.poll() is None:
                second.kill()
                second.wait(timeout=2.0)

    def test_internal_failures_use_failed_and_do_not_poison_later_calls(self):
        self.environment["GISLAND_NOTIFICATIONS_TEST_INTERNAL_FAILURE"] = "1"
        module = self.start()
        proxy = self.proxy()
        with self.assertRaises(GLib.Error) as caught:
            self.notify(proxy, summary="trigger-internal-failure")
        self.assertEqual(self.remote_error(caught.exception), "org.freedesktop.DBus.Error.Failed")
        self.assertEqual(module.collect(), [])
        self.assertEqual(self.notify(proxy, summary="Recovered"), 1)
        self.assertEqual(module.next_record()["context_id"], "notification-1")

    def test_corrupt_history_recovers_without_startup_failure(self):
        path = self.root / "state/gisland/notifications-history.json"
        path.parent.mkdir(parents=True)
        path.write_text("not json\n", encoding="utf-8")
        module = self.start()
        self._show_more()
        publication = module.next_record()
        self.assertIn("Aucune notification", self.text_values(publication["views"]["expanded"]))

    def test_history_startup_rejects_symlinks_and_oversized_files_and_secures_modes(self):
        path = self.root / "state/gisland/notifications-history.json"
        path.parent.mkdir(parents=True, mode=0o777)
        target = self.root / "outside.json"
        target.write_text('{"version":1,"next_sequence":1,"records":[]}\n', encoding="utf-8")
        path.symlink_to(target)
        module = self.start()
        self.assertEqual(stat.S_IMODE(path.parent.stat().st_mode), 0o700)
        self._show_more()
        publication = module.next_record()
        self.assertIn("Aucune notification", self.text_values(publication["views"]["expanded"]))
        module.close()
        self.module = None
        path.unlink()
        with path.open("wb") as output:
            output.seek(1024 + 1000 * (18 * 4096 + 256) + 1)
            output.write(b"x")
        module = self.start()
        self._show_more()
        self.assertIn("Aucune notification", self.text_values(module.next_record()["views"]["expanded"]))

    def test_session_bus_loss_is_fatal_after_ready(self):
        daemon = shutil.which("dbus-daemon")
        if daemon is None:
            self.skipTest("dbus-daemon is unavailable")
        result = subprocess.run(
            [daemon, "--session", "--fork", "--print-address=1", "--print-pid=1"],
            check=True,
            capture_output=True,
            text=True,
        )
        address, pid_text = result.stdout.strip().splitlines()
        environment = self.environment.copy()
        environment["DBUS_SESSION_BUS_ADDRESS"] = address
        isolated = None
        try:
            isolated = ModuleProcess(
                self.command, environment, self.protocol_minor
            )
            os.kill(int(pid_text), 15)
            isolated.process.wait(timeout=3.0)
            self.assertNotEqual(isolated.process.returncode, 0)
        finally:
            if isolated is not None:
                isolated.close()
            try:
                os.kill(int(pid_text), 15)
            except ProcessLookupError:
                pass

    def test_name_owner_loss_is_fatal_while_the_connection_remains_alive(self):
        self.environment["GISLAND_NOTIFICATIONS_TEST_RELEASE_NAME_AFTER_MS"] = "50"
        module = self.start()
        module.process.wait(timeout=3.0)
        self.assertNotEqual(module.process.returncode, 0)
        self.module = None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--module-command", required=True, type=parse_command)
    parser.add_argument("--protocol-minor", required=True, type=int)
    parser.add_argument("--legacy-history-dbus", action="store_true")
    arguments, unittest_arguments = parser.parse_known_args()
    NotificationContract.command = arguments.module_command
    NotificationContract.protocol_minor = arguments.protocol_minor
    NotificationContract.legacy_history_dbus = arguments.legacy_history_dbus
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(NotificationContract)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
