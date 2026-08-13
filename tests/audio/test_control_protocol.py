import tempfile
import unittest
from pathlib import Path

from gisland_audio.control import dispatch, resolve_gislandctl
from gisland_audio.dbus_service import AudioDBusService
from gisland_audio.protocol import ProtocolController


class ControlTest(unittest.TestCase):
    def test_dispatches_supported_commands(self):
        calls = []
        for command in ("mute", "up", "down"):
            dispatch(command, lambda method, value: calls.append((method, value)))
        self.assertEqual(
            calls,
            [("ToggleMute", None), ("ChangeVolume", 5), ("ChangeVolume", -5)],
        )
        with self.assertRaises(ValueError):
            dispatch("louder", lambda _method, _value: None)

    def test_resolves_sibling_then_user_then_path(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            program = root / "bin" / "gisland-audio-control"
            program.parent.mkdir()
            program.write_text("")
            sibling = program.with_name("gislandctl")
            sibling.write_text("")
            sibling.chmod(0o755)
            self.assertEqual(resolve_gislandctl(str(program), root / "home", lambda _name: None), str(sibling))
            sibling.unlink()
            user = root / "home" / ".local" / "bin" / "gislandctl"
            user.parent.mkdir(parents=True)
            user.write_text("")
            user.chmod(0o755)
            self.assertEqual(resolve_gislandctl(str(program), root / "home", lambda _name: "/path/gislandctl"), str(user))
            user.unlink()
            self.assertEqual(resolve_gislandctl(str(program), root / "home", lambda _name: "/path/gislandctl"), "/path/gislandctl")


class ProtocolTest(unittest.TestCase):
    def test_waits_for_init_and_bus_before_protocol_ready(self):
        records = []
        configured = []
        fatal = []
        controller = ProtocolController(records.append, configured.append, lambda: None, fatal.append)
        controller.handle(
            {
                "type": "init",
                "protocol": {"maximum": {"major": 1, "minor": 7}},
                "capabilities": [
                    "independent-views",
                    "compact-view-styles",
                    "icon-roles",
                    "progress-transitions",
                ],
                "configuration": {"step_percent": 5},
            }
        )
        self.assertEqual(records, [])
        controller.bus_ready()
        self.assertEqual(configured, [{"step_percent": 5}])
        self.assertEqual(records[0]["protocol_minor"], 7)
        self.assertEqual(
            records[0]["capabilities"],
            [
                "independent-views",
                "compact-view-styles",
                "icon-roles",
                "progress-transitions",
            ],
        )
        self.assertEqual(fatal, [])

    def test_rejects_missing_capability(self):
        fatal = []
        controller = ProtocolController(lambda _record: None, lambda _config: None, lambda: None, fatal.append)
        controller.handle(
            {
                "type": "init",
                "protocol": {"maximum": {"major": 1, "minor": 7}},
                "capabilities": [],
                "configuration": {},
            }
        )
        self.assertEqual(fatal, ["core does not offer audio capabilities"])


class DBusServiceTest(unittest.TestCase):
    def test_dispatches_calls_serially_and_rejects_zero_delta(self):
        calls = []

        class Service:
            def toggle_mute(self):
                calls.append(("mute",))

            def change_volume(self, value):
                calls.append(("volume", value))

        class Parameters:
            def __init__(self, values):
                self.values = values

            def unpack(self):
                return self.values

        class Invocation:
            def __init__(self):
                self.returned = False
                self.error = None

            def return_value(self, _value):
                self.returned = True

            def return_dbus_error(self, name, message):
                self.error = (name, message)

        dbus = AudioDBusService(lambda: None, lambda _message: None)
        dbus.set_service(Service())
        for method, values in (("ToggleMute", ()), ("ChangeVolume", (5,)), ("ChangeVolume", (-5,))):
            invocation = Invocation()
            dbus._method_call(None, None, None, None, method, Parameters(values), invocation)
            self.assertTrue(invocation.returned)
        self.assertEqual(calls, [("mute",), ("volume", 5), ("volume", -5)])

        invalid = Invocation()
        dbus._method_call(None, None, None, None, "ChangeVolume", Parameters((0,)), invalid)
        self.assertEqual(invalid.error[0], "org.freedesktop.DBus.Error.InvalidArgs")


if __name__ == "__main__":
    unittest.main()
