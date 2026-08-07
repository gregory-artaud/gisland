import json
import os
import tempfile
import unittest
from pathlib import Path

from gisland_notifications.history import HistoryRecord, NotificationHistory
from gisland_notifications.history_scenes import build_history_publication


class NotificationHistoryTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.path = Path(self.temporary.name) / "gisland" / "notifications-history.json"
        self.diagnostics = []

    def tearDown(self):
        self.temporary.cleanup()

    def history(self, limit=100):
        return NotificationHistory(self.path, limit=limit, diagnostic=self.diagnostics.append)

    def test_missing_state_starts_empty_and_round_trips_atomically(self):
        history = self.history()
        self.assertEqual(history.records, ())

        sequence = history.add(7, "Files", "Ready", "archive <b>done</b>", 100.0)

        self.assertEqual(sequence, 1)
        self.assertEqual(os.stat(self.path).st_mode & 0o777, 0o600)
        self.assertEqual(list(self.path.parent.glob("*.tmp")), [])
        loaded = self.history()
        self.assertEqual(loaded.records, history.records)
        self.assertEqual(loaded.records[0].body, "archive done")

    def test_invalid_or_unsupported_state_is_diagnosed_and_ignored(self):
        self.path.parent.mkdir(parents=True)
        for content in ("not json", json.dumps({"version": 2, "records": []})):
            with self.subTest(content=content):
                self.path.write_text(content, encoding="utf-8")
                diagnostics = []
                history = NotificationHistory(self.path, diagnostic=diagnostics.append)
                self.assertEqual(history.records, ())
                self.assertTrue(diagnostics)

    def test_retains_newest_records_and_replacement_moves_one_to_front(self):
        history = self.history(limit=2)
        first = history.add(1, "One", "First", "body", 1.0)
        history.add(2, "Two", "Second", "body", 2.0)
        history.replace(first, 1, "One", "Updated", "new body", 3.0)

        self.assertEqual([record.summary for record in history.records], ["Updated", "Second"])
        self.assertEqual([record.notification_id for record in history.records], [1, 2])

        history.add(3, "Three", "Third", "body", 4.0)
        self.assertEqual([record.summary for record in history.records], ["Third", "Updated"])

    def test_new_notification_id_after_restart_does_not_replace_old_record(self):
        history = self.history()
        history.add(1, "Old", "Before restart", "body", 1.0)

        restarted = self.history()
        restarted.add(1, "New", "After restart", "body", 2.0)

        self.assertEqual(
            [record.summary for record in restarted.records],
            ["After restart", "Before restart"],
        )

    def test_loads_up_to_the_manifest_maximum_before_applying_configuration(self):
        history = self.history(limit=150)
        for index in range(120):
            history.add(index + 1, "App", str(index), "", float(index))

        loaded = NotificationHistory(self.path, limit=1000)
        loaded.configure_limit(150)

        self.assertEqual(len(loaded.records), 120)

    def test_scheduled_saves_are_coalesced_and_leave_notify_nonblocking(self):
        callbacks = []
        history = NotificationHistory(
            self.path,
            diagnostic=self.diagnostics.append,
            schedule_save=callbacks.append,
        )

        history.add(1, "App", "First", "", 1.0)
        history.add(2, "App", "Second", "", 2.0)

        self.assertEqual(len(callbacks), 1)
        self.assertFalse(self.path.exists())
        self.assertFalse(callbacks[0]())
        self.assertTrue(self.path.exists())
        self.assertEqual(len(self.history().records), 2)

    def test_rejects_invalid_limits_and_bounds_loaded_records(self):
        for value in (0, 1001, True, 1.5):
            with self.subTest(value=value):
                with self.assertRaises(ValueError):
                    NotificationHistory(self.path, limit=value)

        self.path.parent.mkdir(parents=True)
        self.path.write_text(
            json.dumps(
                {
                    "version": 1,
                    "next_sequence": 2,
                    "records": [
                        {
                            "sequence": 1,
                            "notification_id": 1,
                            "app_name": "a" * 4097,
                            "summary": "summary",
                            "body": "body",
                            "received_at": 1.0,
                        }
                    ],
                }
            ),
            encoding="utf-8",
        )
        history = self.history()
        self.assertEqual(history.records, ())
        self.assertTrue(self.diagnostics)


class HistorySceneTests(unittest.TestCase):
    @staticmethod
    def record(sequence, app_name, summary, body, received_at):
        return HistoryRecord(sequence, sequence, app_name, summary, body, received_at)

    @staticmethod
    def text_values(node):
        values = []
        if node.get("type") == "text":
            values.append(node["value"])
        for key in ("children",):
            for child in node.get(key, []):
                values.extend(HistorySceneTests.text_values(child))
        if "content" in node and isinstance(node["content"], dict):
            values.extend(HistorySceneTests.text_values(node["content"]))
        return values

    def test_empty_history_has_a_noninteractive_expanded_view(self):
        publication = build_history_publication([], visible_count=1, now=100.0)

        self.assertEqual(publication["context_id"], "history")
        self.assertEqual(publication["priority"], 100)
        self.assertEqual(set(publication["views"]), {"expanded"})
        self.assertNotIn("presentation", publication)
        self.assertEqual(
            self.text_values(publication["views"]["expanded"]),
            ["Notifications", "Aucune notification"],
        )

    def test_shows_newest_entries_with_simple_separators_and_relative_ages(self):
        records = [
            self.record(3, "Chat", "Newest", "hello", 99.0),
            self.record(2, "Files", "Middle", "archive", 40.0),
            self.record(1, "Calendar", "Oldest", "meeting", -7200.0),
        ]

        publication = build_history_publication(records, visible_count=3, now=100.0)
        values = self.text_values(publication["views"]["expanded"])

        newest = next(index for index, value in enumerate(values) if value.startswith("Newest"))
        middle = next(index for index, value in enumerate(values) if value.startswith("Middle"))
        oldest = next(index for index, value in enumerate(values) if value.startswith("Oldest"))
        self.assertLess(newest, middle)
        self.assertLess(middle, oldest)
        self.assertEqual(values.count("----------------"), 2)
        self.assertIn("maintenant", values)
        self.assertIn("1 min", values)
        self.assertIn("2 h", values)

    def test_visible_count_is_bounded_by_records_and_five(self):
        records = [self.record(index, "App", f"Item {index}", "", 0.0) for index in range(8, 0, -1)]

        publication = build_history_publication(records, visible_count=8, now=1.0)
        values = self.text_values(publication["views"]["expanded"])

        self.assertEqual(len([value for value in values if value.startswith("Item ")]), 5)
        self.assertEqual(values.count("----------------"), 4)

    def test_every_generated_text_node_respects_the_core_byte_limit(self):
        record = self.record(1, "ß" * 4096, "s" * 4096, "b" * 4096, 0.0)

        publication = build_history_publication([record], visible_count=1, now=1.0)
        values = self.text_values(publication["views"]["expanded"])

        self.assertTrue(values)
        self.assertTrue(all(len(value.encode("utf-8")) <= 4096 for value in values))


if __name__ == "__main__":
    unittest.main()
