import json
import math
import os
import tempfile
from collections.abc import Callable
from dataclasses import asdict, dataclass
from pathlib import Path
from threading import Lock
from typing import Any

from .markup import parse_body


STATE_VERSION = 1
MAXIMUM_LIMIT = 1000
MAXIMUM_APP_NAME_BYTES = 4096
MAXIMUM_SUMMARY_BYTES = 4096
MAXIMUM_BODY_BYTES = 4096


@dataclass(frozen=True)
class HistoryRecord:
    sequence: int
    notification_id: int
    app_name: str
    summary: str
    body: str
    received_at: float


def state_path(environment: dict[str, str] | None = None) -> Path:
    values = os.environ if environment is None else environment
    state_home = values.get("XDG_STATE_HOME")
    if state_home:
        return Path(state_home) / "gisland" / "notifications-history.json"
    home = values.get("HOME")
    if not home:
        raise ValueError("HOME is not set")
    return Path(home) / ".local" / "state" / "gisland" / "notifications-history.json"


def _bounded_string(value: Any, maximum_bytes: int, name: str) -> str:
    if not isinstance(value, str):
        raise ValueError(f"{name} must be a string")
    if len(value.encode("utf-8")) > maximum_bytes:
        raise ValueError(f"{name} exceeds the byte limit")
    return value


def _truncate_utf8(value: str, maximum_bytes: int) -> str:
    encoded = value.encode("utf-8")[:maximum_bytes]
    while encoded:
        try:
            return encoded.decode("utf-8")
        except UnicodeDecodeError:
            encoded = encoded[:-1]
    return ""


def plain_body(value: str) -> str:
    parsed = parse_body(value)
    parts = []
    for item in parsed.items:
        if item["type"] in ("text", "link"):
            parts.append(item["value"])
        elif item["type"] == "inline_image":
            image = parsed.images.get(item["resource_id"])
            if image is not None and image[1]:
                parts.append(image[1])
    result = "".join(parts)
    return _truncate_utf8(result, MAXIMUM_BODY_BYTES)


class NotificationHistory:
    def __init__(
        self,
        path: Path,
        limit: int = 100,
        diagnostic: Callable[[str], None] = lambda _message: None,
        schedule_save: Callable[[Callable[[], bool]], Any] | None = None,
    ):
        self._validate_limit(limit)
        self._path = path
        self._limit = limit
        self._diagnostic = diagnostic
        self._schedule_save = schedule_save
        self._save_lock = Lock()
        self._pending_document = None
        self._save_task = None
        self._records: list[HistoryRecord] = []
        self._next_sequence = 1
        self._load()

    @staticmethod
    def _validate_limit(limit: int) -> None:
        if type(limit) is not int or not 1 <= limit <= MAXIMUM_LIMIT:
            raise ValueError(f"history_limit must be an integer between 1 and {MAXIMUM_LIMIT}")

    @property
    def records(self) -> tuple[HistoryRecord, ...]:
        return tuple(self._records)

    @property
    def limit(self) -> int:
        return self._limit

    def has_sequence(self, sequence: int) -> bool:
        return any(record.sequence == sequence for record in self._records)

    def configure_limit(self, limit: int) -> None:
        self._validate_limit(limit)
        self._limit = limit
        if len(self._records) > limit:
            del self._records[limit:]
            self._request_save()

    def add(
        self,
        notification_id: int,
        app_name: str,
        summary: str,
        body: str,
        received_at: float,
    ) -> int:
        record = self._record(
            self._next_sequence,
            notification_id,
            _truncate_utf8(app_name, MAXIMUM_APP_NAME_BYTES),
            _truncate_utf8(summary, MAXIMUM_SUMMARY_BYTES),
            plain_body(body),
            received_at,
        )
        self._next_sequence += 1
        self._records.insert(0, record)
        del self._records[self._limit :]
        self._request_save()
        return record.sequence

    def replace(
        self,
        sequence: int,
        notification_id: int,
        app_name: str,
        summary: str,
        body: str,
        received_at: float,
    ) -> int:
        record = self._record(
            sequence,
            notification_id,
            _truncate_utf8(app_name, MAXIMUM_APP_NAME_BYTES),
            _truncate_utf8(summary, MAXIMUM_SUMMARY_BYTES),
            plain_body(body),
            received_at,
        )
        self._records = [candidate for candidate in self._records if candidate.sequence != sequence]
        self._records.insert(0, record)
        del self._records[self._limit :]
        self._request_save()
        return sequence

    @staticmethod
    def _record(
        sequence: Any,
        notification_id: Any,
        app_name: Any,
        summary: Any,
        body: Any,
        received_at: Any,
    ) -> HistoryRecord:
        if type(sequence) is not int or sequence <= 0:
            raise ValueError("sequence must be a positive integer")
        if type(notification_id) is not int or not 1 <= notification_id <= 0xFFFFFFFF:
            raise ValueError("notification_id must be a positive uint32")
        if type(received_at) not in (int, float) or not math.isfinite(received_at):
            raise ValueError("received_at must be finite")
        return HistoryRecord(
            sequence=sequence,
            notification_id=notification_id,
            app_name=_bounded_string(app_name, MAXIMUM_APP_NAME_BYTES, "app_name"),
            summary=_bounded_string(summary, MAXIMUM_SUMMARY_BYTES, "summary"),
            body=_bounded_string(body, MAXIMUM_BODY_BYTES, "body"),
            received_at=float(received_at),
        )

    def _load(self) -> None:
        try:
            if not self._path.exists():
                return
            document = json.loads(self._path.read_text(encoding="utf-8"))
            if not isinstance(document, dict) or set(document) != {
                "version",
                "next_sequence",
                "records",
            }:
                raise ValueError("history state must contain version, next_sequence, and records")
            if document["version"] != STATE_VERSION:
                raise ValueError("unsupported history state version")
            if type(document["next_sequence"]) is not int or document["next_sequence"] <= 0:
                raise ValueError("next_sequence must be a positive integer")
            if not isinstance(document["records"], list) or len(document["records"]) > MAXIMUM_LIMIT:
                raise ValueError("history records exceed the limit")
            records = []
            for value in document["records"]:
                if not isinstance(value, dict) or set(value) != {
                    "sequence",
                    "notification_id",
                    "app_name",
                    "summary",
                    "body",
                    "received_at",
                }:
                    raise ValueError("history record has invalid fields")
                records.append(self._record(**value))
            if len({record.sequence for record in records}) != len(records):
                raise ValueError("history record sequences must be unique")
            if records and document["next_sequence"] <= max(record.sequence for record in records):
                raise ValueError("next_sequence must follow stored records")
            self._records = records[: self._limit]
            self._next_sequence = document["next_sequence"]
        except (OSError, UnicodeError, json.JSONDecodeError, ValueError) as error:
            self._records = []
            self._next_sequence = 1
            self._diagnostic(f"could not load notification history: {error}")

    def _request_save(self) -> None:
        document = {
            "version": STATE_VERSION,
            "next_sequence": self._next_sequence,
            "records": [asdict(record) for record in self._records],
        }
        if self._schedule_save is None:
            self._save(document)
            return
        with self._save_lock:
            self._pending_document = document
            if self._save_task is not None:
                return
            self._save_task = True
        try:
            task = self._schedule_save(self._drain_saves)
            with self._save_lock:
                if self._save_task is True:
                    self._save_task = task if task is not None else True
        except Exception as error:
            with self._save_lock:
                self._save_task = None
            self._diagnostic(f"could not schedule notification history save: {error}")

    def flush(self) -> bool:
        with self._save_lock:
            task = self._save_task
        if task is True:
            self._drain_saves()
        elif task is not None:
            task.result()
        return False

    def _drain_saves(self) -> bool:
        while True:
            with self._save_lock:
                document = self._pending_document
                self._pending_document = None
            if document is not None:
                self._save(document)
            with self._save_lock:
                if self._pending_document is None:
                    self._save_task = None
                    return False

    def _save(self, document: dict[str, Any]) -> None:
        temporary_path = None
        try:
            self._path.parent.mkdir(parents=True, mode=0o700, exist_ok=True)
            os.chmod(self._path.parent, 0o700)
            descriptor, temporary_name = tempfile.mkstemp(
                prefix=f".{self._path.name}.", suffix=".tmp", dir=self._path.parent
            )
            temporary_path = Path(temporary_name)
            with os.fdopen(descriptor, "w", encoding="utf-8") as output:
                json.dump(document, output, ensure_ascii=False, separators=(",", ":"))
                output.write("\n")
                output.flush()
                os.fsync(output.fileno())
            os.chmod(temporary_path, 0o600)
            os.replace(temporary_path, self._path)
            temporary_path = None
        except OSError as error:
            self._diagnostic(f"could not save notification history: {error}")
        finally:
            if temporary_path is not None:
                try:
                    temporary_path.unlink()
                except FileNotFoundError:
                    pass
