import os
import sys
from pathlib import Path
from typing import Any

from .protocol import JsonlTransport, ProtocolController
from .service import BatteryService
from .upower import UPowerSource


def state_path() -> Path:
    root = Path(os.environ.get("XDG_STATE_HOME", Path.home() / ".local" / "state"))
    return root / "gisland" / "battery-cycle.json"


class Application:
    def __init__(self):
        import gi

        gi.require_version("GLib", "2.0")
        from gi.repository import GLib

        self._loop = GLib.MainLoop()
        self._transport = None
        self._exit_code = 0
        self._service = BatteryService(self._write_record, state_path())
        self._controller = ProtocolController(
            self._write_record,
            self._service.configure,
            self._service.ready,
            self._service.action,
            self.stop,
            self._fatal,
        )
        self._transport = JsonlTransport(self._controller.handle, self.stop)
        self._source = UPowerSource(self._service.update)

    def _write_record(self, record: dict[str, Any]) -> None:
        if self._transport is not None:
            self._transport.write_record(record)

    def _fatal(self, message: str) -> None:
        print(f"gisland-battery: {message}", file=sys.stderr)
        self._exit_code = 1
        self.stop()

    def run(self) -> int:
        self._transport.start()
        try:
            self._source.start()
        except Exception as error:
            print(f"gisland-battery: UPower unavailable: {error}", file=sys.stderr)
        self._loop.run()
        return self._exit_code

    def stop(self) -> None:
        if self._transport is not None:
            self._transport.stop()
        self._loop.quit()


def main() -> int:
    try:
        return Application().run()
    except Exception as error:
        print(f"gisland-battery: {error}", file=sys.stderr)
        return 1
