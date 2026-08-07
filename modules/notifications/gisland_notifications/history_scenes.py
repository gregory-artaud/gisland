from typing import Any

from .history import HistoryRecord


HISTORY_CONTEXT_ID = "history"
HISTORY_PRIORITY = 100
MAXIMUM_VISIBLE_HISTORY = 5
MAXIMUM_TEXT_BYTES = 4096


def _bounded_text(value: str) -> str:
    encoded = value.encode("utf-8")[:MAXIMUM_TEXT_BYTES]
    while encoded:
        try:
            return encoded.decode("utf-8")
        except UnicodeDecodeError:
            encoded = encoded[:-1]
    return ""


def _text(value: str, role: str) -> dict[str, Any]:
    return {"type": "text", "value": _bounded_text(value), "role": role}


def _age(received_at: float, now: float) -> str:
    elapsed = max(0, int(now - received_at))
    if elapsed < 60:
        return "maintenant"
    if elapsed < 3600:
        return f"{elapsed // 60} min"
    if elapsed < 86400:
        return f"{elapsed // 3600} h"
    return f"{elapsed // 86400} j"


def _entry(record: HistoryRecord, now: float, session_id: int) -> dict[str, Any]:
    header = {
        "type": "row",
        "gap": "small",
        "children": [
            _text(record.app_name.upper() or "APPLICATION", "caption"),
            {"type": "spacer", "flexible": True},
            _text(_age(record.received_at, now), "caption"),
        ],
    }
    content = record.summary
    if record.body:
        content = f"{content} - {record.body}" if content else record.body
    children = [header]
    if content:
        children.append(_text(content, "body"))
    return {
        "type": "action_region",
        "action_id": f"history:{session_id}:hide:{record.sequence}",
        "accessible_label": f"Masquer {record.summary or record.app_name or 'notification'}",
        "content": {
            "type": "column",
            "alignment": "start",
            "gap": "xsmall",
            "children": children,
        },
    }


def build_history_publication(
    records: list[HistoryRecord] | tuple[HistoryRecord, ...],
    visible_count: int,
    now: float,
    session_id: int,
) -> dict[str, Any]:
    selected = records[: min(max(visible_count, 0), MAXIMUM_VISIBLE_HISTORY)]
    children: list[dict[str, Any]] = [
        {
            "type": "row",
            "gap": "small",
            "children": [
                _text("Notifications", "title"),
                {"type": "spacer", "flexible": True},
                {
                    "type": "action_region",
                    "action_id": f"history:{session_id}:close-all",
                    "accessible_label": "Masquer toutes les notifications",
                    "content": {
                        "type": "icon",
                        "name": "close",
                        "accessible_label": "Masquer toutes les notifications",
                    },
                },
            ],
        }
    ]
    if not selected:
        children.append(_text("Aucune notification", "caption"))
    else:
        for record in selected:
            children.append(_entry(record, now, session_id))
    return {
        "type": "publish",
        "context_id": HISTORY_CONTEXT_ID,
        "priority": HISTORY_PRIORITY,
        "views": {
            "expanded": {
                "type": "column",
                "alignment": "start",
                "gap": "xsmall",
                "children": children,
            }
        },
    }
