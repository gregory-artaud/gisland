import math
import time
from collections.abc import Callable
from pathlib import Path
from typing import Any

from .model import (
    AlertKind,
    AlertState,
    BatteryOptions,
    BatteryReading,
    CycleStateStore,
    snapshot_for,
)


def _compact(snapshot: dict[str, Any]) -> dict[str, Any]:
    return {
        "type": "row",
        "gap": "small",
        "children": [
            {
                "type": "progress",
                "shape": "ring",
                "value": snapshot["level"],
                "state": snapshot["semantic_state"],
            },
            {"type": "text", "value": snapshot["percent_text"], "role": "compact-primary"},
            {"type": "spacer", "flexible": True},
            {
                "type": "text",
                "value": snapshot["estimate_compact"],
                "role": "compact-secondary",
            },
        ],
    }


def _expanded(snapshot: dict[str, Any], dismissible: bool) -> dict[str, Any]:
    rows: list[dict[str, Any]] = [
        {
            "type": "row",
            "gap": "normal",
            "children": [
                {
                    "type": "progress",
                    "shape": "ring",
                    "value": snapshot["level"],
                    "state": snapshot["semantic_state"],
                },
                {"type": "text", "value": snapshot["percent_text"], "role": "title"},
                {"type": "spacer", "flexible": True},
                {"type": "text", "value": snapshot["state_text"], "role": "body"},
            ],
        }
    ]
    for label, key in (
        ("Autonomie", "estimate_detail"),
        ("Santé", "health_text"),
        ("Puissance", "power_text"),
    ):
        rows.append(
            {
                "type": "row",
                "gap": "small",
                "children": [
                    {"type": "text", "value": label, "role": "caption"},
                    {"type": "spacer", "flexible": True},
                    {"type": "text", "value": snapshot[key], "role": "body"},
                ],
            }
        )
    if dismissible:
        rows.append(
            {
                "type": "button",
                "action_id": "dismiss-alert",
                "accessible_label": "Dismiss battery alert",
                "content": {"type": "text", "value": "Dismiss", "role": "button"},
            }
        )
    return {"type": "column", "gap": "normal", "children": rows}


class BatteryService:
    def __init__(
        self,
        write_record: Callable[[dict[str, Any]], None],
        state_path: Path,
        now: Callable[[], float] = time.monotonic,
    ):
        self._write_record = write_record
        self._store = CycleStateStore(state_path)
        self._now = now
        self._options = BatteryOptions()
        self._alerts = AlertState(self._options)
        self._configured = False
        self._pending: BatteryReading | None = None
        self._last_snapshot: dict[str, Any] | None = None
        self._last_publish = 0.0
        self._active_alert: str | None = None

    def configure(self, values: dict[str, Any]) -> None:
        self._options = BatteryOptions.from_mapping(values)
        self._alerts = self._store.load(self._options)
        self._configured = True

    def ready(self) -> None:
        if self._pending is not None:
            self.update(self._pending)

    def update(self, reading: BatteryReading) -> None:
        self._pending = reading
        if not self._configured:
            return
        if not reading.present or not math.isfinite(reading.percentage):
            return
        snapshot = snapshot_for(reading, self._options)
        immediate_keys = ("percent_text", "semantic_state", "state_text")
        immediate = self._last_snapshot is None or any(
            snapshot[key] != self._last_snapshot[key] for key in immediate_keys
        )
        if immediate or self._now() - self._last_publish >= 30.0:
            self._write_record({"type": "data", "value": snapshot})
            self._last_snapshot = snapshot
            self._last_publish = self._now()

        alerts = self._alerts.observe(reading)
        self._store.save(self._alerts)
        for alert in alerts:
            self._publish_alert(alert, snapshot)

    def _publish_alert(self, kind: AlertKind, snapshot: dict[str, Any]) -> None:
        if self._active_alert is not None:
            self._write_record({"type": "dismiss", "context_id": self._active_alert})
        context_id = f"battery-{kind.value}"
        persistent = kind in (AlertKind.persistent, AlertKind.critical)
        priority = {
            AlertKind.plugged: 35,
            AlertKind.unplugged: 35,
            AlertKind.warning: 40,
            AlertKind.persistent: 60,
            AlertKind.critical: 90,
        }[kind]
        record: dict[str, Any] = {
            "type": "publish",
            "context_id": context_id,
            "priority": priority,
            "views": {
                "compact": _compact(snapshot),
                "expanded": _expanded(snapshot, persistent),
            },
            "presentation": {"reveal": "expanded"},
        }
        if persistent:
            self._active_alert = context_id
        else:
            record["expires_in_ms"] = self._options.preview_duration_ms
            record["presentation"]["duration_ms"] = self._options.preview_duration_ms
            self._active_alert = None
        self._write_record(record)

    def action(self, action_id: str) -> bool:
        if action_id != "dismiss-alert" or self._active_alert is None:
            return False
        self._write_record({"type": "dismiss", "context_id": self._active_alert})
        self._active_alert = None
        return True
