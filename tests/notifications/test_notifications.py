import unittest

from gisland_notifications import PROTOCOL_MAJOR, PROTOCOL_MINOR
from gisland_notifications.markup import parse_body
from gisland_notifications.model import CloseReason, NotificationStore, Urgency
from gisland_notifications.scenes import build_publication


class ProtocolTests(unittest.TestCase):
    def test_protocol_version_supports_independent_views(self):
        self.assertEqual((PROTOCOL_MAJOR, PROTOCOL_MINOR), (1, 4))


class NotificationStoreTests(unittest.TestCase):
    def test_allocates_replaces_and_wraps_positive_uint32_ids(self):
        store = NotificationStore(next_id=0xFFFFFFFF)
        first = store.create(
            app_name="Files",
            summary="Ready",
            body="",
            actions=(),
            hints={},
            expire_timeout=-1,
        )
        second = store.create(
            app_name="Files",
            summary="Again",
            body="",
            actions=(),
            hints={},
            expire_timeout=-1,
        )
        replaced = store.create(
            app_name="Files",
            summary="Updated",
            body="",
            actions=(),
            hints={},
            expire_timeout=1200,
            replaces_id=first.id,
        )

        self.assertEqual(first.id, 0xFFFFFFFF)
        self.assertEqual(second.id, 1)
        self.assertEqual(replaced.id, first.id)
        self.assertEqual(store.get(first.id).summary, "Updated")
        self.assertEqual(replaced.timeout_ms, 1200)

    def test_maps_urgency_timeout_and_residency(self):
        store = NotificationStore()
        low = store.create("a", "low", "", (), {"urgency": 0}, -1)
        normal = store.create("a", "normal", "", (), {}, -1)
        critical = store.create(
            "a", "critical", "", (), {"urgency": 2, "resident": True}, -1
        )

        self.assertEqual((low.urgency, low.priority, low.timeout_ms), (Urgency.LOW, 10, 5000))
        self.assertEqual(
            (normal.urgency, normal.priority, normal.timeout_ms),
            (Urgency.NORMAL, 20, 8000),
        )
        self.assertEqual(
            (critical.urgency, critical.priority, critical.timeout_ms, critical.resident),
            (Urgency.CRITICAL, 30, None, True),
        )

    def test_close_is_idempotent_and_returns_reason_once(self):
        store = NotificationStore()
        notification = store.create("a", "summary", "", (), {}, 0)

        closed = store.close(notification.id, CloseReason.CLOSED_BY_CALL)

        self.assertEqual(closed, (notification, CloseReason.CLOSED_BY_CALL))
        self.assertIsNone(store.close(notification.id, CloseReason.EXPIRED))
        self.assertIsNone(store.get(notification.id))


class MarkupTests(unittest.TestCase):
    def test_converts_nested_emphasis_links_and_inline_images(self):
        parsed = parse_body(
            "Before <b>bold <i>and italic</i></b> "
            '<a href="https://example.com">open</a>\n'
            '<img src="file:///tmp/preview.png" alt="Preview"/> after'
        )

        self.assertEqual(parsed.links, {"link-0": "https://example.com"})
        self.assertEqual(parsed.images, {"inline-0": ("file:///tmp/preview.png", "Preview")})
        self.assertIn(
            {"type": "text", "value": "and italic", "emphasis": ["bold", "italic"]},
            parsed.items,
        )
        self.assertIn(
            {
                "type": "link",
                "value": "open",
                "emphasis": [],
                "action_id": "link-0",
                "accessible_label": "open",
            },
            parsed.items,
        )
        self.assertIn(
            {
                "type": "inline_image",
                "resource_id": "inline-0",
                "role": "notification-inline-image",
                "accessible_label": "Preview",
            },
            parsed.items,
        )

    def test_malformed_or_unsafe_markup_falls_back_to_complete_plain_text(self):
        malformed = "Keep <b>all"
        unsafe = '<!DOCTYPE x [<!ENTITY e SYSTEM "file:///etc/passwd">]><b>&e;</b>'

        self.assertEqual(parse_body(malformed).items, [{"type": "text", "value": malformed}])
        self.assertEqual(parse_body(unsafe).items, [{"type": "text", "value": unsafe}])


class SceneTests(unittest.TestCase):
    def test_builds_expanded_only_notification_with_reveal_intent(self):
        store = NotificationStore()
        notification = store.create(
            "Files",
            "Download complete",
            "The <b>archive</b> is ready. <a href=\"https://example.com\">Open folder</a>",
            ("default", "Open", "show", "Show folder"),
            {"urgency": 1},
            5000,
        )
        publication, routing = build_publication(
            notification,
            reveal_duration_ms=2500,
            app_resource={
                "id": "app-image",
                "format": "rgba8",
                "width": 1,
                "height": 1,
                "data": "/wAA/w==",
            },
        )

        self.assertEqual(publication["type"], "publish")
        self.assertEqual(publication["context_id"], f"notification-{notification.id}")
        self.assertEqual(publication["priority"], 20)
        self.assertEqual(set(publication["views"]), {"expanded"})
        self.assertEqual(publication["views"]["expanded"]["type"], "action_region")
        self.assertEqual(
            publication["presentation"], {"reveal": "expanded", "duration_ms": 2500}
        )
        prefix = f"notification-{notification.id}:"
        self.assertEqual(publication["views"]["expanded"]["action_id"], prefix + "default")
        self.assertEqual(routing[prefix + "default"], ("dbus", "default"))
        self.assertEqual(routing[prefix + "action-0"], ("dbus", "show"))
        self.assertEqual(routing[prefix + "link-0"], ("uri", "https://example.com"))

    def test_omits_image_space_and_default_region_when_absent(self):
        store = NotificationStore()
        notification = store.create("", "", "body", (), {}, 0)

        publication, routing = build_publication(notification, reveal_duration_ms=0)

        self.assertNotIn("compact", publication["views"])
        self.assertNotIn("presentation", publication)
        self.assertEqual(publication["views"]["expanded"]["type"], "column")
        prefix = f"notification-{notification.id}:"
        self.assertNotIn(prefix + "default", routing)
        self.assertEqual(routing[prefix + "close"], ("close", "close"))


if __name__ == "__main__":
    unittest.main()
