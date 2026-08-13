import subprocess
import sys
from typing import Any

from .control import resolve_gislandctl
from .dbus_service import AudioDBusService
from .pactl import Pactl
from .protocol import JsonlTransport, ProtocolController
from .service import AudioService


class Application:
    def __init__(self):
        import gi

        gi.require_version("GLib", "2.0")
        from gi.repository import GLib

        self._loop = GLib.MainLoop()
        self._exit_code = 0
        self._stopping = False
        self._transport = None
        self._dbus = AudioDBusService(self._bus_ready, self._fatal)
        self._service = AudioService(self._write_record, Pactl(), self._close_expanded)
        self._dbus.set_service(self._service)
        self._controller = ProtocolController(
            self._write_record, self._service.configure, self.stop, self._fatal
        )
        self._transport = JsonlTransport(self._controller.handle, self.stop)

    def _write_record(self, record: dict[str, Any]) -> None:
        if self._transport is not None:
            self._transport.write_record(record)

    def _bus_ready(self) -> None:
        self._controller.bus_ready()

    def _close_expanded(self) -> None:
        gislandctl = resolve_gislandctl(sys.argv[0])
        if gislandctl is None:
            print("gisland-audio: gislandctl was not found", file=sys.stderr)
            return
        try:
            result = subprocess.run(
                [gislandctl, "close"], capture_output=True, text=True, check=False, timeout=0.5
            )
            if result.returncode != 0:
                print(f"gisland-audio: gislandctl close failed: {result.stderr.strip()}", file=sys.stderr)
        except (OSError, subprocess.TimeoutExpired) as error:
            print(f"gisland-audio: gislandctl close failed: {error}", file=sys.stderr)

    def _fatal(self, message: str) -> None:
        print(f"gisland-audio: {message}", file=sys.stderr)
        self._exit_code = 1
        self.stop()

    def run(self) -> int:
        self._transport.start()
        self._dbus.start()
        self._loop.run()
        return self._exit_code

    def stop(self) -> None:
        if self._stopping:
            return
        self._stopping = True
        self._dbus.stop()
        if self._transport is not None:
            self._transport.stop()
        self._loop.quit()


def main() -> int:
    try:
        return Application().run()
    except Exception as error:
        print(f"gisland-audio: {error}", file=sys.stderr)
        return 1
