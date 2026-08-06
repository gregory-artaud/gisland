from collections.abc import Callable
from typing import Any
from urllib.parse import urlparse

from .images import ImageData, encode_resource, load_image_file, resolve_app_image
from .markup import parse_body
from .model import CloseReason, NotificationStore
from .scenes import build_publication


class NotificationService:
    def __init__(
        self,
        write_record: Callable[[dict[str, Any]], None],
        emit_signal: Callable[[str, int, Any], None],
        launch_uri: Callable[[str], bool],
        schedule: Callable[[int, Callable[[], bool]], Any],
        cancel_timer: Callable[[Any], None],
        resolve_image: Callable[[dict[str, Any], str], ImageData | None] = resolve_app_image,
        resolve_inline: Callable[[str], ImageData | None] = load_image_file,
    ):
        self.store = NotificationStore()
        self._write_record = write_record
        self._emit_signal = emit_signal
        self._launch_uri = launch_uri
        self._schedule = schedule
        self._cancel_timer = cancel_timer
        self._resolve_image = resolve_image
        self._resolve_inline = resolve_inline
        self._timers: dict[int, Any] = {}
        self._routing: dict[int, dict[str, tuple[str, str]]] = {}

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
            notification, app_resource=app_resource, inline_resources=inline_resources
        )
        self._write_record(publication)
        if replacing is not None:
            self._cancel_notification_timer(notification.id)
        self.store.commit(notification)
        self._routing[notification.id] = routing
        if notification.timeout_ms is not None:
            self._timers[notification.id] = self._schedule(
                notification.timeout_ms,
                lambda notification_id=notification.id: self._expire(notification_id),
            )
        return notification.id

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
        self._write_record({"type": "dismiss", "context_id": notification.context_id})
        self._emit_signal("NotificationClosed", notification_id, close_reason)
        return True

    def action(self, action_id: str) -> bool:
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
        for timer in tuple(self._timers.values()):
            self._cancel_timer(timer)
        self._timers.clear()
        self._routing.clear()
