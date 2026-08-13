import unittest

from gisland_audio.model import AudioOptions, AudioState
from gisland_audio.pactl import Pactl, parse_mute, parse_volume
from gisland_audio.service import AudioService


class AudioOptionsTest(unittest.TestCase):
    def test_defaults_and_strict_bounds(self):
        self.assertEqual(AudioOptions.from_mapping({}), AudioOptions(5, 150, 1500))
        self.assertEqual(
            AudioOptions.from_mapping(
                {"step_percent": 25, "maximum_percent": 200, "hud_duration_ms": 100}
            ),
            AudioOptions(25, 200, 100),
        )
        for values in (
            {"unknown": 1},
            {"step_percent": True},
            {"step_percent": 0},
            {"step_percent": 26},
            {"maximum_percent": 99},
            {"maximum_percent": 201},
            {"hud_duration_ms": 99},
            {"hud_duration_ms": 60001},
        ):
            with self.subTest(values=values), self.assertRaises(ValueError):
                AudioOptions.from_mapping(values)


class PactlTest(unittest.TestCase):
    def test_parses_machine_stable_volume_and_mute(self):
        self.assertEqual(
            parse_volume(
                "Volume: front-left: 27525 /  42% / -22.61 dB,   front-right: 27525 / 42% / -22.61 dB\n"
            ),
            42,
        )
        self.assertTrue(parse_mute("Mute: yes\n"))
        self.assertFalse(parse_mute("Mute: no\n"))
        for value in ("", "Volume: unknown", "Mute: maybe"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                parse_mute(value) if value.startswith("Mute") else parse_volume(value)

    def test_uses_exact_default_sink_commands(self):
        outputs = iter(["Volume: mono: 65536 / 100% / 0.00 dB\n", "Mute: no\n"])
        commands = []

        def run(command):
            commands.append(command)
            return next(outputs, "")

        pactl = Pactl(run)
        self.assertEqual(pactl.read_state(), AudioState(100, False))
        pactl.set_volume(105)
        pactl.set_muted(True)
        self.assertEqual(
            commands,
            [
                ["pactl", "get-sink-volume", "@DEFAULT_SINK@"],
                ["pactl", "get-sink-mute", "@DEFAULT_SINK@"],
                ["pactl", "set-sink-volume", "@DEFAULT_SINK@", "105%"],
                ["pactl", "set-sink-mute", "@DEFAULT_SINK@", "1"],
            ],
        )


class FakeMixer:
    def __init__(self, states):
        self.states = iter(states)
        self.calls = []

    def read_state(self):
        self.calls.append(("read",))
        value = next(self.states)
        if isinstance(value, Exception):
            raise value
        return value

    def set_volume(self, value):
        self.calls.append(("volume", value))

    def set_muted(self, value):
        self.calls.append(("mute", value))


class AudioServiceTest(unittest.TestCase):
    def make_service(self, states):
        self.records = []
        self.closed = 0

        def close():
            self.closed += 1

        mixer = FakeMixer(states)
        service = AudioService(self.records.append, mixer, close)
        service.configure({})
        return service, mixer

    def test_toggle_mute_publishes_authoritative_icon_then_closes_expanded(self):
        service, mixer = self.make_service([AudioState(65, False), AudioState(65, True)])
        service.toggle_mute()
        self.assertEqual(mixer.calls, [("read",), ("mute", True), ("read",)])
        self.assertEqual(self.closed, 1)
        publication = self.records[-1]
        self.assertEqual(publication["context_id"], "audio-mute")
        self.assertEqual(publication["priority"], 80)
        self.assertEqual(publication["expires_in_ms"], 1500)
        self.assertEqual(publication["presentation"], {"compact_style": "hud-symbol"})
        self.assertEqual(publication["views"]["compact"]["name"], "volume-muted")
        self.assertEqual(publication["views"]["compact"]["role"], "hud-mute-icon")

    def test_volume_is_clamped_unmuted_and_animated_from_prior_level(self):
        service, mixer = self.make_service([AudioState(149, True), AudioState(150, False)])
        service.change_volume(5)
        self.assertEqual(
            mixer.calls, [("read",), ("mute", False), ("volume", 150), ("read",)]
        )
        publication = self.records[-1]
        self.assertEqual(publication["context_id"], "audio-volume")
        self.assertEqual(publication["presentation"], {"compact_style": "hud-meter"})
        self.assertEqual(
            publication["views"]["compact"]["children"][0]["role"],
            "hud-volume-icon",
        )
        progress = publication["views"]["compact"]["children"][1]
        self.assertAlmostEqual(progress["transition_from"], 149 / 150)
        self.assertEqual(progress["value"], 1.0)

    def test_replaces_previous_hud_and_uses_configured_step(self):
        service, mixer = self.make_service(
            [
                AudioState(50, False),
                AudioState(50, True),
                AudioState(50, True),
                AudioState(43, False),
            ]
        )
        service.configure({"step_percent": 7})
        service.toggle_mute()
        service.change_volume(-7)
        self.assertEqual(mixer.calls[-3:], [("mute", False), ("volume", 43), ("read",)])
        self.assertEqual(self.records[-2], {"type": "dismiss", "context_id": "audio-mute"})

    def test_failure_logs_by_exception_and_publishes_nothing(self):
        service, _mixer = self.make_service([RuntimeError("sink disappeared")])
        with self.assertRaisesRegex(RuntimeError, "sink disappeared"):
            service.toggle_mute()
        self.assertEqual(self.records, [])
        self.assertEqual(self.closed, 0)


if __name__ == "__main__":
    unittest.main()
