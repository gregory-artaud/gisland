import json
import os
from collections import deque
from collections.abc import Callable
from typing import Any

from . import CAPABILITIES, PROTOCOL_MAJOR, PROTOCOL_MINOR


MAXIMUM_RECORD_BYTES = 8 * 1024 * 1024


class LineDecoder:
    def __init__(self):
        self._buffer = bytearray()

    def feed(self, data: bytes) -> list[dict[str, Any]]:
        self._buffer.extend(data)
        records = []
        while True:
            newline = self._buffer.find(b"\n")
            if newline < 0:
                if len(self._buffer) > MAXIMUM_RECORD_BYTES:
                    raise ValueError("JSONL record exceeds the protocol limit")
                return records
            encoded = bytes(self._buffer[:newline])
            del self._buffer[: newline + 1]
            value = json.loads(encoded.decode("utf-8"))
            if not isinstance(value, dict):
                raise ValueError("JSONL record must be an object")
            records.append(value)


class ProtocolController:
    def __init__(
        self,
        write_record: Callable[[dict[str, Any]], None],
        configure: Callable[[dict[str, Any]], None],
        ready: Callable[[], None],
        action: Callable[[str], bool],
        shutdown: Callable[[], None],
        fatal: Callable[[str], None],
    ):
        self._write_record = write_record
        self._configure = configure
        self._on_ready = ready
        self._action = action
        self._shutdown = shutdown
        self._fatal = fatal
        self._initialized = False
        self._ready = False

    @property
    def ready(self) -> bool:
        return self._ready

    def handle(self, record: dict[str, Any]) -> None:
        message_type = record.get("type")
        if message_type == "init" and not self._initialized:
            try:
                maximum = record["protocol"]["maximum"]
                if maximum["major"] != PROTOCOL_MAJOR or maximum["minor"] < PROTOCOL_MINOR:
                    raise ValueError("core does not offer protocol 1.5")
                capabilities = record.get("capabilities", [])
                if not set(CAPABILITIES).issubset(capabilities):
                    raise ValueError("core does not offer battery capabilities")
                configuration = record.get("configuration")
                if not isinstance(configuration, dict):
                    raise ValueError("configuration must be an object")
                self._configure(configuration)
            except (KeyError, TypeError, ValueError) as error:
                self._fatal(str(error))
                return
            self._initialized = True
            self._ready = True
            self._write_record(
                {
                    "type": "ready",
                    "protocol_major": PROTOCOL_MAJOR,
                    "protocol_minor": PROTOCOL_MINOR,
                    "capabilities": list(CAPABILITIES),
                }
            )
            self._on_ready()
            return
        if message_type == "action" and self._ready:
            action_id = record.get("action_id")
            accepted = isinstance(action_id, str) and self._action(action_id)
            self._write_record(
                {
                    "type": "action_result",
                    "action_id": action_id if isinstance(action_id, str) else "",
                    "accepted": accepted,
                }
            )
            return
        if message_type == "shutdown":
            self._shutdown()


class JsonlTransport:
    def __init__(
        self,
        handle_record: Callable[[dict[str, Any]], None],
        handle_eof: Callable[[], None],
        input_fd: int = 0,
        output_fd: int = 1,
    ):
        self._handle_record = handle_record
        self._handle_eof = handle_eof
        self._input_fd = input_fd
        self._output_fd = output_fd
        self._decoder = LineDecoder()
        self._pending: deque[bytearray] = deque()
        self._read_source = 0
        self._write_source = 0
        self._stopped = False

    def start(self) -> None:
        from gi.repository import GLib

        os.set_blocking(self._input_fd, False)
        os.set_blocking(self._output_fd, False)
        self._read_source = GLib.io_add_watch(
            self._input_fd,
            GLib.IOCondition.IN | GLib.IOCondition.HUP | GLib.IOCondition.ERR,
            self._read_ready,
        )

    def _read_ready(self, _source: Any, _condition: Any) -> bool:
        try:
            data = os.read(self._input_fd, 65536)
            if not data:
                self._handle_eof()
                return False
            for record in self._decoder.feed(data):
                self._handle_record(record)
            return True
        except BlockingIOError:
            return True
        except (OSError, UnicodeDecodeError, json.JSONDecodeError, ValueError):
            self._handle_eof()
            return False

    def write_record(self, record: dict[str, Any]) -> None:
        encoded = bytearray(
            (json.dumps(record, ensure_ascii=False, separators=(",", ":")) + "\n").encode()
        )
        self._pending.append(encoded)
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

    def _write_ready(self, _source: Any, _condition: Any) -> bool:
        if self._stopped or self._flush():
            self._write_source = 0
            return False
        return True

    def stop(self) -> None:
        self._stopped = True
        from gi.repository import GLib

        for source in (self._read_source, self._write_source):
            if source:
                GLib.source_remove(source)
