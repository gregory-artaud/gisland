import tempfile
import unittest
from pathlib import Path

from gisland_notifications.history import NotificationHistory
from gisland_notifications.model import CloseReason
from gisland_notifications.service import NotificationService


class NotificationServiceTests(unittest.TestCase):
    def setUp(self):
        self.records = []
        self.signals = []
        self.launched = []
        self.scheduled = []
        self.cancelled = []
        self.temporary = tempfile.TemporaryDirectory()
        self.diagnostics = []
        self.overlay_closes = []
        self.history = NotificationHistory(
            Path(self.temporary.name) / "history.json", diagnostic=self.diagnostics.append
        )
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
            history=self.history,
            wall_time=lambda: 100.0,
            diagnostic=self.diagnostics.append,
            close_overlay=lambda: self.overlay_closes.append(True),
        )

    def tearDown(self):
        self.temporary.cleanup()

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

    def test_configures_the_generic_reveal_intent(self):
        self.service.configure(
            {"reveal_duration_ms": 2500, "history_limit": 20, "history_visible_limit": 3}
        )

        self.notify()

        self.assertEqual(
            self.records[-1]["presentation"],
            {"reveal": "expanded", "duration_ms": 2500},
        )
        self.assertEqual(self.history.limit, 20)

    def test_rejects_invalid_reveal_durations(self):
        for value in (-1, 60001, True, 1.5, "1000"):
            with self.subTest(value=value):
                with self.assertRaisesRegex(ValueError, "reveal_duration_ms"):
                    self.service.configure({"reveal_duration_ms": value})

    def test_rejects_invalid_history_configuration(self):
        for configuration in (
            {"history_limit": 0},
            {"history_limit": 1001},
            {"history_limit": True},
            {"history_visible_limit": 0},
            {"history_visible_limit": 6},
            {"history_visible_limit": 1.5},
            {"history_limit": 3, "history_visible_limit": 4},
        ):
            with self.subTest(configuration=configuration):
                with self.assertRaises(ValueError):
                    self.service.configure(configuration)

    def test_history_survives_close_and_replacement_updates_one_record(self):
        notification_id = self.notify()
        self.service.close(notification_id, CloseReason.EXPIRED)

        self.assertEqual(len(self.history.records), 1)
        self.assertEqual(self.history.records[0].summary, "Ready")
        self.assertNotIn(notification_id, self.service._history_sequences)

        second_id = self.notify()
        self.service.notify(
            app_name="Files",
            replaces_id=second_id,
            app_icon="",
            summary="Updated",
            body="new body",
            actions=(),
            hints={},
            expire_timeout=1000,
        )
        self.assertEqual(len(self.history.records), 2)
        self.assertEqual(self.history.records[0].summary, "Updated")

    def test_show_more_grows_to_five_and_initial_hidden_does_not_reset(self):
        for index in range(6):
            self.service.notify(
                app_name="App",
                replaces_id=0,
                app_icon="",
                summary=f"Item {index}",
                body="",
                actions=(),
                hints={},
                expire_timeout=1000,
            )
        self.records.clear()

        self.assertEqual(self.service.show_more(), 1)
        self.service.visibility("hidden")
        for expected in range(2, 6):
            self.assertEqual(self.service.show_more(), expected)
        self.assertEqual(self.service.show_more(), 5)

        publications = [record for record in self.records if record["type"] == "publish"]
        self.assertEqual(len(publications), 6)
        values = self.text_values(publications[-1]["views"]["expanded"])
        self.assertEqual(len([value for value in values if value.startswith("Item ")]), 5)

    def test_expanded_then_hidden_dismisses_history_and_resets(self):
        self.notify()
        self.records.clear()
        self.service.show_more()
        self.service.visibility("expanded-active")

        self.service.visibility("hidden")

        self.assertEqual(self.records[-1], {"type": "dismiss", "context_id": "history"})
        self.assertEqual(self.service.show_more(), 1)

    def test_history_opened_while_module_is_expanded_still_resets(self):
        self.notify()
        self.service.visibility("expanded-active")
        self.records.clear()

        self.service.show_more()
        self.service.history_opened()
        self.service.visibility("hidden")

        self.assertEqual(self.records[-1], {"type": "dismiss", "context_id": "history"})
        self.assertEqual(self.service.show_more(), 1)

    def test_history_that_never_opens_resets_after_its_deadline(self):
        self.notify()
        self.records.clear()

        self.service.show_more()
        timeout, callback = self.scheduled[-1]
        self.assertEqual(timeout, 2000)
        self.assertFalse(callback())

        self.assertEqual(self.records[-1], {"type": "dismiss", "context_id": "history"})
        self.assertEqual(self.service.show_more(), 1)

    def test_history_inactivity_closes_after_eight_seconds(self):
        self.notify()
        self.records.clear()
        self.service.show_more()
        self.service.history_opened()

        timeout, callback = self.scheduled[-1]
        self.assertEqual(timeout, 8000)
        self.assertFalse(callback())

        self.assertEqual(self.records[-1], {"type": "dismiss", "context_id": "history"})
        self.assertEqual(self.overlay_closes, [True])

    def test_click_masks_only_the_current_entry_without_backfill(self):
        for index in range(4):
            self.service.notify(
                app_name="App",
                replaces_id=0,
                app_icon="",
                summary=f"Item {index}",
                body="",
                actions=(),
                hints={},
                expire_timeout=1000,
            )
        self.records.clear()
        for _ in range(3):
            self.service.show_more()
            self.service.history_opened()
        selected = self.history.records[:3]
        original_records = self.history.records
        self.records.clear()

        session_id = self.service._history_session_id
        self.assertTrue(
            self.service.action(f"history:{session_id}:hide:{selected[1].sequence}")
        )

        values = self.text_values(self.records[-1]["views"]["expanded"])
        self.assertTrue(any(value.startswith("Item 3") for value in values))
        self.assertTrue(any(value.startswith("Item 1") for value in values))
        self.assertFalse(any(value.startswith("Item 2") for value in values))
        self.assertFalse(any(value.startswith("Item 0") for value in values))
        self.assertEqual(self.history.records, original_records)

        self.service.show_more()
        values = self.text_values(self.records[-1]["views"]["expanded"])
        self.assertTrue(any(value.startswith("Item 0") for value in values))

    def test_masking_the_last_entry_or_close_all_returns_to_compact(self):
        self.notify()
        sequence = self.history.records[0].sequence
        self.records.clear()
        self.service.show_more()
        self.service.history_opened()

        session_id = self.service._history_session_id
        self.assertTrue(self.service.action(f"history:{session_id}:hide:{sequence}"))

        self.assertEqual(self.records[-1], {"type": "dismiss", "context_id": "history"})
        self.assertEqual(self.overlay_closes, [True])
        self.assertEqual(len(self.history.records), 1)

        self.service.show_more()
        self.service.history_opened()
        session_id = self.service._history_session_id
        self.assertTrue(self.service.action(f"history:{session_id}:close-all"))
        self.assertEqual(self.records[-1], {"type": "dismiss", "context_id": "history"})
        self.assertEqual(self.overlay_closes, [True, True])

    def test_stale_timers_and_actions_cannot_mutate_a_newer_session(self):
        self.notify()
        sequence = self.history.records[0].sequence
        self.records.clear()

        self.service.show_more()
        stale_open_callback = self.scheduled[-1][1]
        first_session = self.service._history_session_id
        self.service.show_more()
        self.assertFalse(stale_open_callback())
        self.assertNotEqual(self.records[-1]["type"], "dismiss")

        self.service.history_opened()
        stale_inactivity_callback = self.scheduled[-1][1]
        self.service.visibility("hidden")
        self.service.show_more()
        self.service.history_opened()
        second_session = self.service._history_session_id

        self.assertNotEqual(first_session, second_session)
        self.assertFalse(stale_inactivity_callback())
        self.assertFalse(self.service.action(f"history:{first_session}:hide:{sequence}"))
        self.assertFalse(self.service.action(f"history:{first_session}:close-all"))
        self.assertEqual(self.overlay_closes, [])

    def test_new_arrival_updates_open_history_without_duplicate_reveal(self):
        self.notify()
        self.notify()
        self.records.clear()
        self.service.show_more()
        self.service.show_more()
        self.service.visibility("expanded-active")
        self.records.clear()

        self.service.notify(
            app_name="Chat",
            replaces_id=0,
            app_icon="",
            summary="Newest",
            body="message",
            actions=(),
            hints={},
            expire_timeout=1000,
        )

        self.assertEqual([record["type"] for record in self.records], ["publish", "publish"])
        self.assertNotIn("presentation", self.records[0])
        history_values = self.text_values(self.records[1]["views"]["expanded"])
        self.assertTrue(any(value.startswith("Newest") for value in history_values))
        self.assertEqual(len([value for value in history_values if value.startswith("Ready")]), 1)

    @staticmethod
    def text_values(node):
        values = []
        if node.get("type") == "text":
            values.append(node["value"])
        for child in node.get("children", []):
            values.extend(NotificationServiceTests.text_values(child))
        if isinstance(node.get("content"), dict):
            values.extend(NotificationServiceTests.text_values(node["content"]))
        return values

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
