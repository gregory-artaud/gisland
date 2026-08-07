import json
import os
from collections import deque
from collections.abc import Callable
from typing import Any

from . import CAPABILITIES, PROTOCOL_MAJOR, PROTOCOL_MINOR


MAXIMUM_RECORD_BYTES = 8 * 1024 * 1024
MAXIMUM_PENDING_MESSAGES = 256
MAXIMUM_PENDING_BYTES = 16 * 1024 * 1024


class LineDecoder:
    def __init__(self, maximum_bytes: int = MAXIMUM_RECORD_BYTES):
        self._maximum_bytes = maximum_bytes
        self._buffer = bytearray()

    def feed(self, data: bytes) -> list[dict[str, Any]]:
        self._buffer.extend(data)
        records = []
        while True:
            newline = self._buffer.find(b"\n")
            if newline < 0:
                if len(self._buffer) > self._maximum_bytes:
                    raise ValueError("JSONL record exceeds the protocol limit")
                break
            if newline > self._maximum_bytes:
                raise ValueError("JSONL record exceeds the protocol limit")
            encoded = bytes(self._buffer[:newline])
            del self._buffer[: newline + 1]
            try:
                decoded = encoded.decode("utf-8")
                record = json.loads(decoded)
            except (UnicodeDecodeError, json.JSONDecodeError) as error:
                raise ValueError(f"invalid JSONL record: {error}") from error
            if not isinstance(record, dict):
                raise ValueError("JSONL record must be an object")
            records.append(record)
        return records


class ProtocolController:
    def __init__(
        self,
        write_record: Callable[[dict[str, Any]], None],
        action: Callable[[str], bool],
        shutdown: Callable[[], None],
        fatal: Callable[[str], None],
    ):
        self._write_record = write_record
        self._action = action
        self._shutdown = shutdown
        self._fatal = fatal
        self._initialized = False
        self._bus_ready = False
        self._ready_sent = False
        self._shutdown_sent = False

    def _maybe_ready(self) -> None:
        if not self._initialized or not self._bus_ready or self._ready_sent:
            return
        self._ready_sent = True
        self._write_record(
            {
                "type": "ready",
                "protocol_major": PROTOCOL_MAJOR,
                "protocol_minor": PROTOCOL_MINOR,
                "capabilities": list(CAPABILITIES),
            }
        )

    def bus_ready(self) -> None:
        self._bus_ready = True
        self._maybe_ready()

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
                self._fatal("core does not offer notification capabilities")
                return
            self._initialized = True
            self._maybe_ready()
            return
        if message_type == "action" and self._ready_sent:
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
        if message_type == "shutdown" and not self._shutdown_sent:
            self._shutdown_sent = True
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
        self._pending_bytes = 0
        self._read_source = 0
        self._write_source = 0
        self._stopped = False

    def start(self) -> None:
        import gi

        gi.require_version("GLib", "2.0")
        from gi.repository import GLib

        os.set_blocking(self._input_fd, False)
        os.set_blocking(self._output_fd, False)
        self._read_source = GLib.io_add_watch(
            self._input_fd,
            GLib.IOCondition.IN | GLib.IOCondition.HUP | GLib.IOCondition.ERR,
            self._read_ready,
        )

    def _read_ready(self, _source: Any, condition: Any) -> bool:
        if self._stopped:
            return False
        try:
            data = os.read(self._input_fd, 65536)
        except BlockingIOError:
            return True
        except OSError:
            self._handle_eof()
            return False
        if not data:
            self._handle_eof()
            return False
        try:
            for record in self._decoder.feed(data):
                self._handle_record(record)
        except ValueError as error:
            self.write_record({"type": "log", "level": "error", "message": str(error)})
        return True

    def write_record(self, record: dict[str, Any]) -> None:
        encoded = (json.dumps(record, ensure_ascii=False, separators=(",", ":")) + "\n").encode(
            "utf-8"
        )
        if len(encoded) > MAXIMUM_RECORD_BYTES + 1:
            raise ValueError("outbound JSONL record exceeds the protocol limit")
        if len(self._pending) >= MAXIMUM_PENDING_MESSAGES:
            raise ValueError("outbound JSONL queue exceeds the message limit")
        if self._pending_bytes + len(encoded) > MAXIMUM_PENDING_BYTES:
            raise ValueError("outbound JSONL queue exceeds the byte limit")
        self._pending.append(bytearray(encoded))
        self._pending_bytes += len(encoded)
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
            if written == 0:
                return False
            del self._pending[0][:written]
            self._pending_bytes -= written
            if not self._pending[0]:
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
        self._read_source = 0
        self._write_source = 0
