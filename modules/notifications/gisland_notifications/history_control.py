import json
import os
import shutil
import subprocess
import sys
import time
from collections.abc import Callable
from pathlib import Path
from typing import Any

from .dbus_service import BUS_NAME, HISTORY_INTERFACE_NAME, HISTORY_OBJECT_PATH


def _selected_history(status: Any) -> bool:
    if not isinstance(status, dict) or status.get("format_version") != 2:
        raise RuntimeError("invalid status response")
    expanded = status.get("expanded")
    if expanded is None:
        return False
    if not isinstance(expanded, dict):
        raise RuntimeError("invalid status response")
    return (
        expanded.get("instance_id") == "notifications"
        and expanded.get("context_id") == "history"
    )


def open_history(
    gislandctl: str,
    show_more: Callable[[], int],
    confirm_visible: Callable[[], None],
    run: Callable[[list[str]], subprocess.CompletedProcess[str]],
    monotonic: Callable[[], float] = time.monotonic,
    sleep: Callable[[float], None] = time.sleep,
    timeout: float = 0.5,
) -> int:
    visible_count = show_more()
    deadline = monotonic() + timeout
    activated = False
    while True:
        result = run([gislandctl, "status", "--json"])
        if result.returncode != 0:
            raise RuntimeError(f"gislandctl status failed: {result.stderr.strip()}")
        try:
            status = json.loads(result.stdout)
        except json.JSONDecodeError as error:
            raise RuntimeError("invalid status response") from error
        if _selected_history(status):
            break
        if not activated:
            result = run([gislandctl, "activate-open", "notifications"])
            if result.returncode != 0:
                raise RuntimeError(f"gislandctl activate failed: {result.stderr.strip()}")
            activated = True
        if monotonic() >= deadline:
            raise RuntimeError("timed out waiting for notification history")
        sleep(0.02)

    result = run([gislandctl, "open"])
    if result.returncode != 0:
        raise RuntimeError(f"gislandctl open failed: {result.stderr.strip()}")
    confirm_visible()
    return visible_count


def _history_call(method: str) -> tuple[Any, ...] | None:
    import gi

    gi.require_version("Gio", "2.0")
    from gi.repository import Gio

    proxy = Gio.DBusProxy.new_for_bus_sync(
        Gio.BusType.SESSION,
        Gio.DBusProxyFlags.NONE,
        None,
        BUS_NAME,
        HISTORY_OBJECT_PATH,
        HISTORY_INTERFACE_NAME,
        None,
    )
    result = proxy.call_sync(method, None, Gio.DBusCallFlags.NONE, 500, None)
    return None if result is None else result.unpack()


def _show_more() -> int:
    result = _history_call("ShowMore")
    if result is None:
        raise RuntimeError("ShowMore returned no result")
    return int(result[0])


def _confirm_visible() -> None:
    _history_call("ConfirmVisible")


def resolve_gislandctl(
    program: str, path_lookup: Callable[[str], str | None] = shutil.which
) -> str | None:
    sibling = Path(program).resolve().with_name("gislandctl")
    if sibling.is_file() and os.access(sibling, os.X_OK):
        return str(sibling)
    return path_lookup("gislandctl")


def main() -> int:
    gislandctl = resolve_gislandctl(sys.argv[0])
    if gislandctl is None:
        print("gisland-notification-history: gislandctl was not found", file=sys.stderr)
        return 1
    try:
        open_history(
            gislandctl,
            show_more=_show_more,
            confirm_visible=_confirm_visible,
            run=lambda arguments: subprocess.run(
                arguments, capture_output=True, text=True, check=False, timeout=0.25
            ),
        )
    except Exception as error:
        print(f"gisland-notification-history: {error}", file=sys.stderr)
        return 1
    return 0
