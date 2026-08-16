import json
import os
import subprocess
import tempfile
import time
import unittest
from pathlib import Path


COMMAND_LUA = Path(__file__).parents[2] / "assets" / "modules" / "audio" / "command.lua"
FIXTURES = Path(__file__).parent / "fixtures"
LUA = os.environ.get("GISLAND_LUA_EXECUTABLE", "lua5.4")

RUNNER = r'''
local command = assert(loadfile(os.getenv("GISLAND_AUDIO_COMMAND_LUA")))()
local action = os.getenv("GISLAND_AUDIO_TEST_ACTION")
local result

if action == "get_volume" then
  result = command.get_sink_volume()
elseif action == "get_mute" then
  result = command.get_sink_mute()
elseif action == "set_volume" then
  result = command.set_sink_volume(73)
elseif action == "set_mute_true" then
  result = command.set_sink_mute(true)
elseif action == "set_mute_zero" then
  result = command.set_sink_mute(0)
elseif action == "close" then
  result = command.close()
elseif action == "invalid" then
  local injection = "5%; touch " .. os.getenv("GISLAND_AUDIO_INJECTION_MARKER")
  local checks = {
    function() command.set_sink_volume("5") end,
    function() command.set_sink_volume(1.5) end,
    function() command.set_sink_volume(injection) end,
    function() command.set_sink_mute("1") end,
    function() command.set_sink_mute(2) end,
    function() command.set_sink_mute(injection) end,
    function() command.run("pactl", "anything") end,
  }
  for _, check in ipairs(checks) do
    if pcall(check) then
      io.stderr:write("invalid command input was accepted\n")
      os.exit(3)
    end
  end
  os.exit(0)
else
  error("unknown test action")
end

io.write(result.ok and "1" or "0", "\t", result.reason, "\t",
         tostring(result.status), "\t", result.timed_out and "1" or "0", "\n")
io.write(result.output)
'''


class LuaCommandTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.bin_path = self.root / "bin"
        self.bin_path.mkdir()
        for program in ("pactl", "gislandctl"):
            (self.bin_path / program).symlink_to(FIXTURES / f"fake-{program}")
        self.state_path = self.root / "state.json"
        self.log_path = self.root / "commands.jsonl"
        self.injection_marker = self.root / "injected"
        self.write_state(
            {
                "volume_reads": [[42]],
                "mute_reads": [False],
            }
        )

    def tearDown(self):
        self.temporary.cleanup()

    def write_state(self, state):
        self.state_path.write_text(json.dumps(state), encoding="utf-8")

    def run_helper(self, action, extra_environment=None, timeout=4):
        environment = os.environ.copy()
        environment.pop("GISLAND_LUA_HOST_BINDIR", None)
        environment.update(
            {
                "PATH": f"{self.bin_path}:{environment['PATH']}",
                "HOME": str(self.root),
                "GISLAND_AUDIO_COMMAND_LUA": str(COMMAND_LUA),
                "GISLAND_AUDIO_TEST_ACTION": action,
                "GISLAND_AUDIO_FAKE_STATE": str(self.state_path),
                "GISLAND_AUDIO_COMMAND_LOG": str(self.log_path),
                "GISLAND_AUDIO_INJECTION_MARKER": str(self.injection_marker),
            }
        )
        environment.update(extra_environment or {})
        completed = subprocess.run(
            [LUA, "-"],
            input=RUNNER,
            capture_output=True,
            text=True,
            encoding="utf-8",
            env=environment,
            cwd=self.root,
            check=False,
            timeout=timeout,
        )
        return completed

    def result(self, action, extra_environment=None):
        completed = self.run_helper(action, extra_environment)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        header, output = completed.stdout.split("\n", 1)
        ok, reason, status, timed_out = header.split("\t")
        return {
            "ok": ok == "1",
            "reason": reason,
            "status": int(status),
            "timed_out": timed_out == "1",
            "output": output,
        }

    def command_log(self):
        if not self.log_path.exists():
            return []
        return [
            json.loads(line)
            for line in self.log_path.read_text(encoding="utf-8").splitlines()
        ]

    def test_fixed_templates_and_locale(self):
        self.assertTrue(self.result("get_volume")["ok"])
        self.assertTrue(self.result("get_mute")["ok"])
        self.assertTrue(self.result("set_volume")["ok"])
        self.assertTrue(self.result("set_mute_true")["ok"])
        self.assertTrue(self.result("set_mute_zero")["ok"])
        self.assertTrue(self.result("close")["ok"])
        self.assertEqual(
            [(entry["program"], entry["argv"]) for entry in self.command_log()],
            [
                ("pactl", ["get-sink-volume", "@DEFAULT_SINK@"]),
                ("pactl", ["get-sink-mute", "@DEFAULT_SINK@"]),
                ("pactl", ["set-sink-volume", "@DEFAULT_SINK@", "73%"]),
                ("pactl", ["set-sink-mute", "@DEFAULT_SINK@", "1"]),
                ("pactl", ["set-sink-mute", "@DEFAULT_SINK@", "0"]),
                ("gislandctl", ["close"]),
            ],
        )
        pactl_entries = [
            entry for entry in self.command_log() if entry["program"] == "pactl"
        ]
        self.assertEqual({entry["lc_all"] for entry in pactl_entries}, {"C"})

    def test_rejects_noninteger_metacharacters_and_arbitrary_commands(self):
        completed = self.run_helper("invalid")
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(self.command_log(), [])
        self.assertFalse(self.injection_marker.exists())

    def test_merges_diagnostics_and_reports_exit_status(self):
        self.write_state(
            {
                "volume_reads": [[42]],
                "mute_reads": [False],
                "fail_once": "get-sink-volume @DEFAULT_SINK@",
                "failure_message": "sink disappeared",
            }
        )
        result = self.result("get_volume")
        self.assertFalse(result["ok"])
        self.assertEqual(result["reason"], "exit")
        self.assertEqual(result["status"], 1)
        self.assertFalse(result["timed_out"])
        self.assertIn("sink disappeared", result["output"])

    def test_captured_output_is_bounded(self):
        self.write_state(
            {
                "volume_reads": [[42]],
                "mute_reads": [False],
                "fail_once": "get-sink-volume @DEFAULT_SINK@",
                "failure_message": "x" * 100_000,
            }
        )
        result = self.result("get_volume")
        self.assertFalse(result["ok"])
        self.assertEqual(len(result["output"]), 64 * 1024)

    def test_pactl_timeout_is_bounded_and_reported(self):
        self.write_state(
            {
                "volume_reads": [[42]],
                "mute_reads": [False],
                "sleep_once": "get-sink-volume @DEFAULT_SINK@",
                "sleep_seconds": 3,
            }
        )
        started = time.monotonic()
        result = self.result("get_volume")
        elapsed = time.monotonic() - started
        self.assertFalse(result["ok"])
        self.assertEqual(result["status"], 124)
        self.assertTrue(result["timed_out"])
        self.assertGreaterEqual(elapsed, 1.8)
        self.assertLess(elapsed, 2.8)

    def test_pactl_timeout_kills_a_process_that_ignores_sigterm(self):
        self.write_state(
            {
                "volume_reads": [[42]],
                "mute_reads": [False],
                "ignore_sigterm_once": "get-sink-volume @DEFAULT_SINK@",
            }
        )
        started = time.monotonic()
        result = self.result("get_volume")
        elapsed = time.monotonic() - started
        self.assertFalse(result["ok"])
        self.assertEqual(result["status"], 137)
        self.assertTrue(result["timed_out"])
        self.assertGreaterEqual(elapsed, 1.8)
        self.assertLess(elapsed, 3.0)

    def test_gislandctl_timeout_is_bounded_and_reported(self):
        started = time.monotonic()
        result = self.result(
            "close", {"GISLAND_AUDIO_FAKE_GISLANDCTL_SLEEP": "1"}
        )
        elapsed = time.monotonic() - started
        self.assertFalse(result["ok"])
        self.assertEqual(result["status"], 124)
        self.assertTrue(result["timed_out"])
        self.assertGreaterEqual(elapsed, 0.4)
        self.assertLess(elapsed, 1.2)

    def test_gislandctl_timeout_kills_a_process_that_ignores_sigterm(self):
        started = time.monotonic()
        result = self.result(
            "close", {"GISLAND_AUDIO_FAKE_GISLANDCTL_IGNORE_SIGTERM": "1"}
        )
        elapsed = time.monotonic() - started
        self.assertFalse(result["ok"])
        self.assertEqual(result["status"], 137)
        self.assertTrue(result["timed_out"])
        self.assertGreaterEqual(elapsed, 0.4)
        self.assertLess(elapsed, 1.5)

    def test_close_finds_home_local_gislandctl_when_path_excludes_it(self):
        home = self.root / "home'; touch injected; #"
        local_bin = home / ".local" / "bin"
        local_bin.mkdir(parents=True)
        (local_bin / "gislandctl").symlink_to(FIXTURES / "fake-gislandctl")
        (self.bin_path / "gislandctl").unlink()

        result = self.result("close", {"HOME": str(home)})

        self.assertTrue(result["ok"])
        self.assertEqual(
            [(entry["program"], entry["argv"]) for entry in self.command_log()],
            [("gislandctl", ["close"])],
        )
        self.assertFalse(self.injection_marker.exists())

    def test_close_prefers_executable_host_sibling_when_path_excludes_it(self):
        host_bin = self.root / "host'; touch injected; #" / "bin"
        host_bin.mkdir(parents=True)
        (host_bin / "gislandctl").symlink_to(FIXTURES / "fake-gislandctl")
        (self.bin_path / "gislandctl").unlink()

        result = self.result(
            "close",
            {
                "GISLAND_LUA_HOST_BINDIR": str(host_bin),
                "PATH": f"{self.bin_path}:/usr/bin:/bin",
            },
        )

        self.assertTrue(result["ok"])
        self.assertEqual(
            [(entry["program"], entry["argv"]) for entry in self.command_log()],
            [("gislandctl", ["close"])],
        )
        self.assertFalse(self.injection_marker.exists())

    def test_non_executable_home_candidate_falls_through_to_path(self):
        local_bin = self.root / ".local" / "bin"
        local_bin.mkdir(parents=True)
        candidate = local_bin / "gislandctl"
        candidate.write_text("#!/bin/sh\nexit 99\n", encoding="utf-8")
        candidate.chmod(0o644)

        result = self.result("close")

        self.assertTrue(result["ok"])
        self.assertEqual(
            [(entry["program"], entry["argv"]) for entry in self.command_log()],
            [("gislandctl", ["close"])],
        )


if __name__ == "__main__":
    unittest.main()
