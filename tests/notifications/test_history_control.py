import json
import tempfile
import subprocess
import unittest
from pathlib import Path

from gisland_notifications.history_control import open_history, resolve_gislandctl


class HistoryControlTests(unittest.TestCase):
    def test_resolves_an_installed_sibling_before_path(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            gislandctl = directory / "gislandctl"
            gislandctl.touch(mode=0o700)

            resolved = resolve_gislandctl(
                str(directory / "gisland-notification-history"),
                path_lookup=lambda _name: "/fallback/gislandctl",
            )

        self.assertEqual(resolved, str(gislandctl))

    def test_falls_back_to_path_when_the_sibling_is_absent(self):
        self.assertEqual(
            resolve_gislandctl(
                "/missing/gisland-notification-history",
                path_lookup=lambda name: f"/fallback/{name}",
            ),
            "/fallback/gislandctl",
        )

    def test_waits_for_history_then_opens(self):
        calls = []
        confirmations = []
        statuses = iter(
            [
                {"format_version": 2, "expanded": None},
                {
                    "format_version": 2,
                    "expanded": {
                        "instance_id": "notifications",
                        "context_id": "history",
                        "priority": 100,
                    },
                },
            ]
        )

        def run(arguments):
            calls.append(arguments)
            if arguments[-2:] == ["status", "--json"]:
                return subprocess.CompletedProcess(arguments, 0, json.dumps(next(statuses)), "")
            return subprocess.CompletedProcess(arguments, 0, "", "")

        count = open_history(
            "/usr/bin/gislandctl",
            show_more=lambda: 2,
            confirm_visible=lambda: confirmations.append(True),
            run=run,
            monotonic=iter((0.0, 0.1, 0.2)).__next__,
            sleep=lambda _duration: None,
        )

        self.assertEqual(count, 2)
        self.assertEqual(calls[-1], ["/usr/bin/gislandctl", "open"])
        self.assertEqual(confirmations, [True])

    def test_rejects_malformed_status_and_bounded_timeout(self):
        with self.assertRaisesRegex(RuntimeError, "invalid status"):
            open_history(
                "gislandctl",
                show_more=lambda: 1,
                confirm_visible=lambda: None,
                run=lambda arguments: subprocess.CompletedProcess(arguments, 0, "not json", ""),
                monotonic=lambda: 0.0,
                sleep=lambda _duration: None,
            )

        times = iter((0.0, 0.1, 0.6))
        with self.assertRaisesRegex(RuntimeError, "timed out"):
            open_history(
                "gislandctl",
                show_more=lambda: 1,
                confirm_visible=lambda: None,
                run=lambda arguments: subprocess.CompletedProcess(
                    arguments, 0, '{"format_version":2,"expanded":null}', ""
                ),
                monotonic=times.__next__,
                sleep=lambda _duration: None,
                timeout=0.5,
            )

    def test_reports_status_and_open_failures(self):
        with self.assertRaisesRegex(RuntimeError, "status failed"):
            open_history(
                "gislandctl",
                show_more=lambda: 1,
                confirm_visible=lambda: None,
                run=lambda arguments: subprocess.CompletedProcess(arguments, 1, "", "offline"),
            )

        def run(arguments):
            if arguments[-2:] == ["status", "--json"]:
                status = {
                    "format_version": 2,
                    "expanded": {
                        "instance_id": "notifications",
                        "context_id": "history",
                        "priority": 100,
                    },
                }
                return subprocess.CompletedProcess(arguments, 0, json.dumps(status), "")
            return subprocess.CompletedProcess(arguments, 1, "", "open rejected")

        with self.assertRaisesRegex(RuntimeError, "open failed"):
            open_history(
                "gislandctl", show_more=lambda: 1, confirm_visible=lambda: None, run=run
            )


if __name__ == "__main__":
    unittest.main()
