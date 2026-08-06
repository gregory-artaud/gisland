import sys
from typing import Any

from .dbus_service import NotificationDBusService
from .protocol import JsonlTransport, ProtocolController
from .service import NotificationService


class Application:
    def __init__(self, version: str = "development"):
        import gi

        gi.require_version("Gio", "2.0")
        gi.require_version("GLib", "2.0")
        from gi.repository import Gio, GLib

        self._Gio = Gio
        self._GLib = GLib
        self._loop = GLib.MainLoop()
        self._exit_code = 0
        self._stopping = False
        self._transport = None
        self._controller = None
        self._dbus = NotificationDBusService(self._bus_ready, self._fatal, version)
        self._service = NotificationService(
            write_record=self._write_record,
            emit_signal=self._dbus.emit_signal,
            launch_uri=self._launch_uri,
            schedule=lambda timeout, callback: GLib.timeout_add(timeout, callback),
            cancel_timer=GLib.source_remove,
        )
        self._dbus.set_service(self._service)
        self._controller = ProtocolController(
            write_record=self._write_record,
            action=self._service.action,
            shutdown=self.stop,
            fatal=self._fatal,
        )
        self._transport = JsonlTransport(self._controller.handle, self.stop)

    def _write_record(self, record: dict[str, Any]) -> None:
        if self._transport is not None:
            self._transport.write_record(record)

    def _bus_ready(self) -> None:
        if self._controller is not None:
            self._controller.bus_ready()

    def _launch_uri(self, uri: str) -> bool:
        try:
            return bool(self._Gio.AppInfo.launch_default_for_uri(uri, None))
        except Exception as error:
            self._write_record({"type": "log", "level": "error", "message": str(error)})
            return False

    def _fatal(self, message: str) -> None:
        self._exit_code = 1
        self._write_record({"type": "log", "level": "error", "message": message})
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
        self._service.shutdown()
        self._dbus.stop()
        if self._transport is not None:
            self._transport.stop()
        self._loop.quit()


def main(version: str = "development") -> int:
    try:
        return Application(version).run()
    except Exception as error:
        print(f"gisland-notifications: {error}", file=sys.stderr)
        return 1
