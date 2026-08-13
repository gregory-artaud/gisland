import json
import os
from collections import deque
from typing import Any

from . import CAPABILITIES, PROTOCOL_MAJOR, PROTOCOL_MINOR


class ProtocolController:
    def __init__(self, write_record, configure, shutdown, fatal):
        self._write_record = write_record
        self._configure = configure
        self._shutdown = shutdown
        self._fatal = fatal
        self._initialized = False
        self._bus_ready = False
        self._ready = False

    @property
    def ready(self) -> bool:
        return self._ready

    def bus_ready(self) -> None:
        self._bus_ready = True
        self._maybe_ready()

    def _maybe_ready(self) -> None:
        if not self._initialized or not self._bus_ready or self._ready:
            return
        self._ready = True
        self._write_record(
            {
                "type": "ready",
                "protocol_major": PROTOCOL_MAJOR,
                "protocol_minor": PROTOCOL_MINOR,
                "capabilities": list(CAPABILITIES),
            }
        )

    def handle(self, record: dict[str, Any]) -> None:
        message_type = record.get("type")
        if message_type == "init":
            if self._initialized:
                self._fatal("duplicate init message")
                return
            protocol = record.get("protocol", {})
            maximum = protocol.get("maximum", {}) if isinstance(protocol, dict) else {}
            if maximum.get("major") != PROTOCOL_MAJOR or maximum.get("minor", -1) < PROTOCOL_MINOR:
                self._fatal(f"core does not offer protocol {PROTOCOL_MAJOR}.{PROTOCOL_MINOR}")
                return
            capabilities = record.get("capabilities")
            if not isinstance(capabilities, list) or not set(CAPABILITIES).issubset(capabilities):
                self._fatal("core does not offer audio capabilities")
                return
            configuration = record.get("configuration")
            if not isinstance(configuration, dict):
                self._fatal("configuration must be an object")
                return
            try:
                self._configure(configuration)
            except ValueError as error:
                self._fatal(str(error))
                return
            self._initialized = True
            self._maybe_ready()
        elif message_type == "shutdown":
            self._shutdown()


class JsonlTransport:
    def __init__(self, handle_record, handle_eof, input_fd: int = 0, output_fd: int = 1):
        self._handle_record = handle_record
        self._handle_eof = handle_eof
        self._input_fd = input_fd
        self._output_fd = output_fd
        self._buffer = bytearray()
        self._pending: deque[bytearray] = deque()
        self._read_source = 0
        self._write_source = 0

    def start(self) -> None:
        from gi.repository import GLib

        os.set_blocking(self._input_fd, False)
        os.set_blocking(self._output_fd, False)
        self._read_source = GLib.io_add_watch(
            self._input_fd,
            GLib.IOCondition.IN | GLib.IOCondition.HUP | GLib.IOCondition.ERR,
            self._read_ready,
        )

    def _read_ready(self, _source, _condition) -> bool:
        try:
            data = os.read(self._input_fd, 65536)
            if not data:
                self._handle_eof()
                return False
            self._buffer.extend(data)
            while (newline := self._buffer.find(b"\n")) >= 0:
                encoded = bytes(self._buffer[:newline])
                del self._buffer[: newline + 1]
                record = json.loads(encoded.decode("utf-8"))
                if not isinstance(record, dict):
                    raise ValueError("JSONL record must be an object")
                self._handle_record(record)
            return True
        except (OSError, UnicodeDecodeError, json.JSONDecodeError, ValueError):
            self._handle_eof()
            return False

    def write_record(self, record: dict[str, Any]) -> None:
        self._pending.append(
            bytearray((json.dumps(record, separators=(",", ":")) + "\n").encode("utf-8"))
        )
        if not self._flush() and self._write_source == 0:
            from gi.repository import GLib

            self._write_source = GLib.io_add_watch(
                self._output_fd, GLib.IOCondition.OUT | GLib.IOCondition.ERR, self._write_ready
            )

    def _flush(self) -> bool:
        while self._pending:
            try:
                written = os.write(self._output_fd, self._pending[0])
            except BlockingIOError:
                return False
            del self._pending[0][:written]
            if self._pending[0]:
                return False
            self._pending.popleft()
        return True

    def _write_ready(self, _source, _condition) -> bool:
        if self._flush():
            self._write_source = 0
            return False
        return True

    def stop(self) -> None:
        from gi.repository import GLib

        for source in (self._read_source, self._write_source):
            if source:
                GLib.source_remove(source)
