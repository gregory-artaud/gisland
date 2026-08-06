import unittest

from gisland_notifications.model import CloseReason
from gisland_notifications.service import NotificationService


class NotificationServiceTests(unittest.TestCase):
    def setUp(self):
        self.records = []
        self.signals = []
        self.launched = []
        self.scheduled = []
        self.cancelled = []
        self.service = NotificationService(
            write_record=self.records.append,
            emit_signal=lambda name, notification_id, value: self.signals.append(
                (name, notification_id, value)
            ),
            launch_uri=lambda uri: self.launched.append(uri) or True,
            schedule=lambda timeout, callback: self.scheduled.append((timeout, callback)) or len(
                self.scheduled
            ),
            cancel_timer=self.cancelled.append,
            resolve_image=lambda hints, app_icon: None,
            resolve_inline=lambda source: None,
        )

    def notify(self, resident=False, replaces_id=0):
        return self.service.notify(
            app_name="Files",
            replaces_id=replaces_id,
            app_icon="",
            summary="Ready",
            body='<a href="https://example.com">Open site</a>',
            actions=("default", "Open", "show", "Show"),
            hints={"resident": resident},
            expire_timeout=1000,
        )

    def test_notify_publishes_and_replacement_rearms_timer(self):
        notification_id = self.notify()
        replaced_id = self.notify(replaces_id=notification_id)

        self.assertEqual(replaced_id, notification_id)
        self.assertEqual([record["type"] for record in self.records], ["publish", "publish"])
        self.assertEqual([timeout for timeout, _ in self.scheduled], [1000, 1000])
        self.assertEqual(self.cancelled, [1])

    def test_failed_replacement_preserves_live_notification_and_timer(self):
        notification_id = self.notify()
        original = self.service.store.get(notification_id)

        self.service._write_record = lambda _record: (_ for _ in ()).throw(
            ValueError("record too large")
        )
        with self.assertRaisesRegex(ValueError, "record too large"):
            self.notify(replaces_id=notification_id)

        self.assertIs(self.service.store.get(notification_id), original)
        self.assertEqual(self.cancelled, [])
        self.service._write_record = self.records.append
        self.assertTrue(self.service.action(f"notification-{notification_id}:action-0"))

    def test_named_action_emits_signal_and_closes_nonresident_notification(self):
        notification_id = self.notify()
        accepted = self.service.action(f"notification-{notification_id}:action-0")

        self.assertTrue(accepted)
        self.assertIn(("ActionInvoked", notification_id, "show"), self.signals)
        self.assertIn(("NotificationClosed", notification_id, CloseReason.DISMISSED), self.signals)
        self.assertEqual(self.records[-1]["type"], "dismiss")

    def test_resident_action_remains_live(self):
        notification_id = self.notify(resident=True)

        self.assertTrue(self.service.action(f"notification-{notification_id}:default"))
        self.assertIsNotNone(self.service.store.get(notification_id))
        self.assertEqual(self.signals, [("ActionInvoked", notification_id, "default")])

    def test_link_launch_does_not_emit_dbus_action_or_close(self):
        notification_id = self.notify()

        self.assertTrue(self.service.action(f"notification-{notification_id}:link-0"))
        self.assertEqual(self.launched, ["https://example.com"])
        self.assertEqual(self.signals, [])
        self.assertIsNotNone(self.service.store.get(notification_id))

    def test_close_and_expire_emit_exact_reasons_once(self):
        closed_id = self.notify()
        expired_id = self.notify()

        self.assertTrue(self.service.close(closed_id, CloseReason.CLOSED_BY_CALL))
        self.assertFalse(self.service.close(closed_id, CloseReason.EXPIRED))
        self.scheduled[-1][1]()

        self.assertIn(
            ("NotificationClosed", closed_id, CloseReason.CLOSED_BY_CALL), self.signals
        )
        self.assertIn(("NotificationClosed", expired_id, CloseReason.EXPIRED), self.signals)

    def test_close_action_ignores_residency(self):
        notification_id = self.notify(resident=True)

        self.assertTrue(self.service.action(f"notification-{notification_id}:close"))
        self.assertIsNone(self.service.store.get(notification_id))
        self.assertIn(("NotificationClosed", notification_id, CloseReason.DISMISSED), self.signals)

    def test_rejects_unknown_stale_and_unsafe_actions(self):
        notification_id = self.notify()

        self.assertFalse(self.service.action(f"notification-{notification_id}:action-99"))
        self.assertFalse(self.service.action("notification-999:close"))


if __name__ == "__main__":
    unittest.main()
