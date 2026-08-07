import unittest

from gisland_notifications.protocol import JsonlTransport, LineDecoder, ProtocolController


class LineDecoderTests(unittest.TestCase):
    def test_decodes_partial_and_multiple_jsonl_records(self):
        decoder = LineDecoder(maximum_bytes=64)

        self.assertEqual(decoder.feed(b'{"a":'), [])
        self.assertEqual(decoder.feed(b"1}\n{\"b\":2}\n"), [{"a": 1}, {"b": 2}])

    def test_rejects_invalid_utf8_json_and_oversized_records(self):
        decoder = LineDecoder(maximum_bytes=8)
        with self.assertRaises(ValueError):
            decoder.feed(b"\xff\n")
        with self.assertRaises(ValueError):
            LineDecoder(maximum_bytes=8).feed(b"123456789")
        with self.assertRaises(ValueError):
            LineDecoder().feed(b"not-json\n")


class JsonlTransportTests(unittest.TestCase):
    def test_bounds_pending_output_messages(self):
        transport = JsonlTransport(lambda _record: None, lambda: None)
        transport._flush = lambda: False
        transport._write_source = 1

        for index in range(256):
            transport.write_record({"type": "log", "message": str(index)})

        with self.assertRaisesRegex(ValueError, "message limit"):
            transport.write_record({"type": "log", "message": "overflow"})


class ProtocolControllerTests(unittest.TestCase):
    def setUp(self):
        self.records = []
        self.actions = []
        self.shutdowns = []
        self.failures = []
        self.configurations = []
        self.visibilities = []
        self.controller = ProtocolController(
            write_record=self.records.append,
            action=lambda action_id: self.actions.append(action_id) or action_id.endswith("close"),
            shutdown=lambda: self.shutdowns.append(True),
            fatal=self.failures.append,
            configure=self.configurations.append,
            visibility=self.visibilities.append,
        )

    @staticmethod
    def init(
        maximum_minor=4,
        capabilities=("context-images", "rich-content", "independent-views"),
    ):
        return {
            "type": "init",
            "protocol": {
                "minimum": {"major": 1, "minor": 0},
                "maximum": {"major": 1, "minor": maximum_minor},
            },
            "instance_id": "notifications",
            "capabilities": list(capabilities),
            "configuration": {},
            "locale": "C",
            "timezone": "UTC",
        }

    def test_sends_ready_only_after_init_and_bus_ownership(self):
        message = self.init()
        message["configuration"] = {"reveal_duration_ms": 2500}
        self.controller.handle(message)
        self.assertEqual(self.records, [])
        self.assertEqual(self.configurations, [{"reveal_duration_ms": 2500}])

        self.controller.bus_ready()

        self.assertEqual(
            self.records,
            [
                {
                    "type": "ready",
                    "protocol_major": 1,
                    "protocol_minor": 4,
                    "capabilities": ["context-images", "rich-content", "independent-views"],
                }
            ],
        )

    def test_rejects_incompatible_init(self):
        self.controller.bus_ready()

        self.controller.handle(self.init(maximum_minor=3))

        self.assertEqual(self.records, [])
        self.assertEqual(self.failures, ["core does not offer protocol 1.4"])

    def test_routes_actions_and_returns_one_result(self):
        self.controller.handle(self.init())
        self.controller.bus_ready()

        self.controller.handle({"type": "action", "action_id": "notification-1:close"})

        self.assertEqual(self.actions, ["notification-1:close"])
        self.assertEqual(
            self.records[-1],
            {
                "type": "action_result",
                "action_id": "notification-1:close",
                "accepted": True,
            },
        )

    def test_shutdown_is_forwarded_once(self):
        self.controller.handle({"type": "shutdown", "deadline_ms": 1000})
        self.controller.handle({"type": "shutdown", "deadline_ms": 1000})

        self.assertEqual(self.shutdowns, [True])

    def test_routes_valid_visibility_only_after_ready(self):
        self.controller.handle({"type": "visibility", "visibility": "hidden"})
        self.controller.handle(self.init())
        self.controller.bus_ready()

        for value in ("hidden", "compact-active", "expanded-active"):
            self.controller.handle({"type": "visibility", "visibility": value})
        self.controller.handle({"type": "visibility", "visibility": "invalid"})

        self.assertEqual(
            self.visibilities,
            ["hidden", "compact-active", "expanded-active"],
        )


if __name__ == "__main__":
    unittest.main()
