from collections.abc import Callable
import time
from typing import Any
from urllib.parse import urlparse

from .history import NotificationHistory
from .history_scenes import HISTORY_CONTEXT_ID, build_history_publication
from .images import ImageData, encode_resource, load_image_file, resolve_app_image
from .markup import parse_body
from .model import CloseReason, NotificationStore
from .scenes import build_publication


HISTORY_OPEN_TIMEOUT_MS = 2000
HISTORY_INACTIVITY_TIMEOUT_MS = 8000


class NotificationService:
    def __init__(
        self,
        write_record: Callable[[dict[str, Any]], None],
        emit_signal: Callable[[str, int, Any], None],
        launch_uri: Callable[[str], bool],
        schedule: Callable[[int, Callable[[], bool]], Any],
        cancel_timer: Callable[[Any], None],
        history: NotificationHistory,
        resolve_image: Callable[[dict[str, Any], str], ImageData | None] = resolve_app_image,
        resolve_inline: Callable[[str], ImageData | None] = load_image_file,
        wall_time: Callable[[], float] = time.time,
        diagnostic: Callable[[str], None] = lambda _message: None,
        close_overlay: Callable[[], None] = lambda: None,
    ):
        self.store = NotificationStore()
        self._write_record = write_record
        self._emit_signal = emit_signal
        self._launch_uri = launch_uri
        self._schedule = schedule
        self._cancel_timer = cancel_timer
        self._resolve_image = resolve_image
        self._resolve_inline = resolve_inline
        self._history = history
        self._wall_time = wall_time
        self._diagnostic = diagnostic
        self._close_overlay = close_overlay
        self._timers: dict[int, Any] = {}
        self._routing: dict[int, dict[str, tuple[str, str]]] = {}
        self._history_sequences: dict[int, int] = {}
        self._reveal_duration_ms = 1000
        self._history_visible_limit = 5
        self._history_visible_count = 0
        self._history_was_expanded = False
        self._visibility = "hidden"
        self._history_open_timer = None
        self._history_inactivity_timer = None
        self._history_open_generation = 0
        self._history_inactivity_generation = 0
        self._history_session_id = 0
        self._history_hidden_sequences: set[int] = set()

    def configure(self, configuration: dict[str, Any]) -> None:
        reveal_duration_ms = configuration.get("reveal_duration_ms", 1000)
        if type(reveal_duration_ms) is not int or not 0 <= reveal_duration_ms <= 60000:
            raise ValueError("reveal_duration_ms must be an integer between 0 and 60000")
        history_limit = configuration.get("history_limit", 100)
        if type(history_limit) is not int or not 1 <= history_limit <= 1000:
            raise ValueError("history_limit must be an integer between 1 and 1000")
        history_visible_limit = configuration.get("history_visible_limit", 5)
        if type(history_visible_limit) is not int or not 1 <= history_visible_limit <= 5:
            raise ValueError("history_visible_limit must be an integer between 1 and 5")
        if history_visible_limit > history_limit:
            raise ValueError("history_visible_limit must not exceed history_limit")
        self._reveal_duration_ms = reveal_duration_ms
        self._history.configure_limit(history_limit)
        self._history_visible_limit = history_visible_limit

    def notify(
        self,
        app_name: str,
        replaces_id: int,
        app_icon: str,
        summary: str,
        body: str,
        actions: tuple[str, ...],
        hints: dict[str, Any],
        expire_timeout: int,
    ) -> int:
        replacing = self.store.get(replaces_id)
        notification = self.store.prepare(
            app_name,
            summary,
            body,
            actions,
            hints,
            expire_timeout,
            replaces_id,
        )
        app_image = self._resolve_image(hints, app_icon)
        app_resource = encode_resource("app-image", app_image) if app_image is not None else None
        parsed = parse_body(body)
        inline_resources: dict[str, dict[str, Any]] = {}
        for resource_id, (source, _label) in parsed.images.items():
            try:
                image = self._resolve_inline(source)
            except (OSError, ValueError):
                image = None
            if image is not None:
                inline_resources[resource_id] = encode_resource(resource_id, image)

        publication, routing = build_publication(
            notification,
            reveal_duration_ms=(
                0 if self._history_visible_count > 0 else self._reveal_duration_ms
            ),
            app_resource=app_resource,
            inline_resources=inline_resources,
        )
        self._write_record(publication)
        if replacing is not None:
            self._cancel_notification_timer(notification.id)
        self.store.commit(notification)
        self._routing[notification.id] = routing
        sequence = self._history_sequences.get(notification.id) if replacing is not None else None
        received_at = self._wall_time()
        if sequence is not None and self._history.has_sequence(sequence):
            self._history.replace(
                sequence,
                notification.id,
                notification.app_name,
                notification.summary,
                notification.body,
                received_at,
            )
        else:
            sequence = self._history.add(
                notification.id,
                notification.app_name,
                notification.summary,
                notification.body,
                received_at,
            )
        self._history_sequences[notification.id] = sequence
        if self._history_visible_count > 0:
            try:
                self._write_record(self._history_publication(self._history_visible_count))
            except ValueError as error:
                self._diagnostic(f"could not update notification history: {error}")
        if notification.timeout_ms is not None:
            self._timers[notification.id] = self._schedule(
                notification.timeout_ms,
                lambda notification_id=notification.id: self._expire(notification_id),
            )
        return notification.id

    def _history_publication(self, visible_count: int) -> dict[str, Any]:
        records = tuple(
            record
            for record in self._history.records
            if record.sequence not in self._history_hidden_sequences
        )
        return build_history_publication(
            records,
            visible_count=visible_count,
            now=self._wall_time(),
            session_id=self._history_session_id,
        )

    def show_more(self) -> int:
        if self._history_visible_count == 0:
            self._history_session_id += 1
            self._history_hidden_sequences.clear()
        visible_count = min(self._history_visible_count + 1, self._history_visible_limit)
        self._write_record(self._history_publication(visible_count))
        self._history_visible_count = visible_count
        self._cancel_history_inactivity_timer()
        self._cancel_history_open_timer()
        self._history_open_timer = self._schedule(
            HISTORY_OPEN_TIMEOUT_MS,
            lambda generation=self._history_open_generation: self._history_open_timeout(generation),
        )
        return visible_count

    def history_opened(self) -> None:
        if self._history_visible_count == 0:
            raise ValueError("notification history is not pending")
        self._cancel_history_open_timer()
        self._history_was_expanded = True
        self._rearm_history_inactivity_timer()

    def visibility(self, visibility: str) -> None:
        self._visibility = visibility
        if visibility == "expanded-active" and self._history_visible_count > 0:
            self.history_opened()
        elif visibility == "hidden" and self._history_was_expanded:
            self._cancel_history_open_timer()
            self._cancel_history_inactivity_timer()
            self._reset_history_session()

    def _cancel_history_open_timer(self) -> None:
        self._history_open_generation += 1
        if self._history_open_timer is not None:
            self._cancel_timer(self._history_open_timer)
            self._history_open_timer = None

    def _history_open_timeout(self, generation: int) -> bool:
        if generation != self._history_open_generation:
            return False
        self._history_open_timer = None
        if self._history_visible_count > 0 and not self._history_was_expanded:
            self._close_history()
        return False

    def _cancel_history_inactivity_timer(self) -> None:
        self._history_inactivity_generation += 1
        if self._history_inactivity_timer is not None:
            self._cancel_timer(self._history_inactivity_timer)
            self._history_inactivity_timer = None

    def _rearm_history_inactivity_timer(self) -> None:
        self._cancel_history_inactivity_timer()
        self._history_inactivity_timer = self._schedule(
            HISTORY_INACTIVITY_TIMEOUT_MS,
            lambda generation=self._history_inactivity_generation: self._history_inactivity_timeout(
                generation
            ),
        )

    def _history_inactivity_timeout(self, generation: int) -> bool:
        if generation != self._history_inactivity_generation:
            return False
        self._history_inactivity_timer = None
        self._close_history()
        return False

    def _reset_history_session(self) -> None:
        self._write_record({"type": "dismiss", "context_id": HISTORY_CONTEXT_ID})
        self._history_visible_count = 0
        self._history_was_expanded = False
        self._history_hidden_sequences.clear()

    def _close_history(self) -> None:
        self._cancel_history_open_timer()
        self._cancel_history_inactivity_timer()
        self._reset_history_session()
        self._close_overlay()

    def _history_action(self, action_id: str) -> bool:
        if self._history_visible_count == 0:
            return False
        parts = action_id.split(":")
        if len(parts) < 3 or parts[0] != "history":
            return False
        try:
            session_id = int(parts[1])
        except ValueError:
            return False
        if session_id != self._history_session_id:
            return False
        if parts[2:] == ["close-all"]:
            self._close_history()
            return True
        if len(parts) != 4 or parts[2] != "hide":
            return False
        try:
            sequence = int(parts[3])
        except ValueError:
            return False
        visible_records = [
            record
            for record in self._history.records
            if record.sequence not in self._history_hidden_sequences
        ][: self._history_visible_count]
        if not any(record.sequence == sequence for record in visible_records):
            return False
        self._history_hidden_sequences.add(sequence)
        self._history_visible_count -= 1
        if self._history_visible_count == 0:
            self._close_history()
        else:
            self._write_record(self._history_publication(self._history_visible_count))
            self._rearm_history_inactivity_timer()
        return True

    def _cancel_notification_timer(self, notification_id: int) -> None:
        timer = self._timers.pop(notification_id, None)
        if timer is not None:
            self._cancel_timer(timer)

    def _expire(self, notification_id: int) -> bool:
        self._timers.pop(notification_id, None)
        self.close(notification_id, CloseReason.EXPIRED)
        return False

    def close(self, notification_id: int, reason: CloseReason) -> bool:
        closed = self.store.close(notification_id, reason)
        if closed is None:
            return False
        notification, close_reason = closed
        self._cancel_notification_timer(notification_id)
        self._routing.pop(notification_id, None)
        self._history_sequences.pop(notification_id, None)
        self._write_record({"type": "dismiss", "context_id": notification.context_id})
        self._emit_signal("NotificationClosed", notification_id, close_reason)
        return True

    def action(self, action_id: str) -> bool:
        if action_id.startswith("history:"):
            return self._history_action(action_id)
        prefix, separator, _name = action_id.partition(":")
        if not separator or not prefix.startswith("notification-"):
            return False
        try:
            notification_id = int(prefix.removeprefix("notification-"))
        except ValueError:
            return False
        notification = self.store.get(notification_id)
        route = self._routing.get(notification_id, {}).get(action_id)
        if notification is None or route is None:
            return False
        kind, value = route
        if kind == "close":
            return self.close(notification_id, CloseReason.DISMISSED)
        if kind == "uri":
            parsed = urlparse(value)
            if parsed.scheme not in ("http", "https", "mailto"):
                return False
            return self._launch_uri(value)
        if kind != "dbus":
            return False
        self._emit_signal("ActionInvoked", notification_id, value)
        if not notification.resident:
            self.close(notification_id, CloseReason.DISMISSED)
        return True

    def shutdown(self) -> None:
        self._cancel_history_open_timer()
        self._cancel_history_inactivity_timer()
        for timer in tuple(self._timers.values()):
            self._cancel_timer(timer)
        self._timers.clear()
        self._routing.clear()
        self._history.flush()
