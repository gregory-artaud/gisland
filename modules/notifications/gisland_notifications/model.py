from dataclasses import dataclass
from enum import IntEnum
from typing import Any


class Urgency(IntEnum):
    LOW = 0
    NORMAL = 1
    CRITICAL = 2


class CloseReason(IntEnum):
    EXPIRED = 1
    DISMISSED = 2
    CLOSED_BY_CALL = 3
    UNDEFINED = 4


@dataclass(frozen=True)
class Notification:
    id: int
    app_name: str
    summary: str
    body: str
    actions: tuple[str, ...]
    hints: dict[str, Any]
    urgency: Urgency
    priority: int
    timeout_ms: int | None
    resident: bool

    @property
    def context_id(self) -> str:
        return f"notification-{self.id}"


class NotificationStore:
    def __init__(self, next_id: int = 1):
        if not 1 <= next_id <= 0xFFFFFFFF:
            raise ValueError("next_id must be a positive uint32")
        self._next_id = next_id
        self._live: dict[int, Notification] = {}

    def _allocate_id(self) -> int:
        if len(self._live) >= 0xFFFFFFFF:
            raise RuntimeError("notification ID space exhausted")
        candidate = self._next_id
        while candidate in self._live:
            candidate = 1 if candidate == 0xFFFFFFFF else candidate + 1
        self._next_id = 1 if candidate == 0xFFFFFFFF else candidate + 1
        return candidate

    @staticmethod
    def _urgency(hints: dict[str, Any]) -> Urgency:
        value = hints.get("urgency", Urgency.NORMAL)
        if value == Urgency.LOW:
            return Urgency.LOW
        if value == Urgency.CRITICAL:
            return Urgency.CRITICAL
        return Urgency.NORMAL

    @staticmethod
    def _timeout(expire_timeout: int, urgency: Urgency) -> int | None:
        if expire_timeout > 0:
            return expire_timeout
        if expire_timeout == 0 or urgency == Urgency.CRITICAL:
            return None
        return 5000 if urgency == Urgency.LOW else 8000

    def prepare(
        self,
        app_name: str,
        summary: str,
        body: str,
        actions: tuple[str, ...],
        hints: dict[str, Any],
        expire_timeout: int,
        replaces_id: int = 0,
    ) -> Notification:
        if len(actions) % 2 != 0:
            raise ValueError("actions must contain key and label pairs")
        notification_id = replaces_id if replaces_id in self._live else self._allocate_id()
        urgency = self._urgency(hints)
        priority = {Urgency.LOW: 10, Urgency.NORMAL: 20, Urgency.CRITICAL: 30}[urgency]
        notification = Notification(
            id=notification_id,
            app_name=app_name,
            summary=summary,
            body=body,
            actions=tuple(actions),
            hints=dict(hints),
            urgency=urgency,
            priority=priority,
            timeout_ms=self._timeout(expire_timeout, urgency),
            resident=hints.get("resident") is True,
        )
        return notification

    def commit(self, notification: Notification) -> None:
        self._live[notification.id] = notification

    def create(
        self,
        app_name: str,
        summary: str,
        body: str,
        actions: tuple[str, ...],
        hints: dict[str, Any],
        expire_timeout: int,
        replaces_id: int = 0,
    ) -> Notification:
        notification = self.prepare(
            app_name, summary, body, actions, hints, expire_timeout, replaces_id
        )
        self.commit(notification)
        return notification

    def get(self, notification_id: int) -> Notification | None:
        return self._live.get(notification_id)

    def close(
        self, notification_id: int, reason: CloseReason
    ) -> tuple[Notification, CloseReason] | None:
        notification = self._live.pop(notification_id, None)
        if notification is None:
            return None
        return notification, reason
