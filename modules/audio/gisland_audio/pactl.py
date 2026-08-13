import os
import re
import subprocess
from collections.abc import Callable

from .model import AudioState


CommandRunner = Callable[[list[str]], str]


def parse_volume(output: str) -> int:
    values = [int(value) for value in re.findall(r"/\s*(\d+)%", output)]
    if not values:
        raise ValueError("pactl returned an invalid sink volume")
    return round(sum(values) / len(values))


def parse_mute(output: str) -> bool:
    match = re.fullmatch(r"\s*Mute:\s*(yes|no)\s*", output)
    if match is None:
        raise ValueError("pactl returned an invalid sink mute state")
    return match.group(1) == "yes"


def run_pactl(command: list[str]) -> str:
    environment = os.environ.copy()
    environment["LC_ALL"] = "C"
    completed = subprocess.run(
        command,
        check=True,
        capture_output=True,
        text=True,
        timeout=2.0,
        env=environment,
    )
    return completed.stdout


class Pactl:
    def __init__(self, run: CommandRunner = run_pactl):
        self._run = run

    def read_state(self) -> AudioState:
        volume = parse_volume(self._run(["pactl", "get-sink-volume", "@DEFAULT_SINK@"]))
        muted = parse_mute(self._run(["pactl", "get-sink-mute", "@DEFAULT_SINK@"]))
        return AudioState(volume, muted)

    def set_volume(self, percent: int) -> None:
        self._run(["pactl", "set-sink-volume", "@DEFAULT_SINK@", f"{percent}%"])

    def set_muted(self, muted: bool) -> None:
        self._run(["pactl", "set-sink-mute", "@DEFAULT_SINK@", "1" if muted else "0"])
