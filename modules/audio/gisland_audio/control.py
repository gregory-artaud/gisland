import os
import shutil
import sys
from collections.abc import Callable
from pathlib import Path

from .dbus_service import BUS_NAME, INTERFACE_NAME, OBJECT_PATH


def dispatch(command: str, call: Callable[[str, int | None], None]) -> None:
    if command == "mute":
        call("ToggleMute", None)
    elif command == "up":
        call("ChangeVolume", 5)
    elif command == "down":
        call("ChangeVolume", -5)
    else:
        raise ValueError("expected mute, up, or down")


def resolve_gislandctl(
    program: str,
    home: Path = Path.home(),
    path_lookup: Callable[[str], str | None] = shutil.which,
) -> str | None:
    for candidate in (
        Path(program).resolve().with_name("gislandctl"),
        home / ".local" / "bin" / "gislandctl",
    ):
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return str(candidate)
    return path_lookup("gislandctl")


def _dbus_call(method: str, value: int | None) -> None:
    import gi

    gi.require_version("Gio", "2.0")
    from gi.repository import Gio, GLib

    parameters = None if value is None else GLib.Variant("(i)", (value,))
    connection = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    connection.call_sync(
        BUS_NAME,
        OBJECT_PATH,
        INTERFACE_NAME,
        method,
        parameters,
        None,
        Gio.DBusCallFlags.NONE,
        2000,
        None,
    )


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: gisland-audio-control {mute|up|down}", file=sys.stderr)
        return 2
    try:
        dispatch(sys.argv[1], _dbus_call)
    except Exception as error:
        print(f"gisland-audio-control: {error}", file=sys.stderr)
        return 1
    return 0
