from typing import Any

from .markup import parse_body
from .model import Notification


def _text(value: str, role: str) -> dict[str, Any]:
    return {"type": "text", "value": value, "role": role}


def build_publication(
    notification: Notification,
    app_resource: dict[str, Any] | None = None,
    inline_resources: dict[str, dict[str, Any]] | None = None,
) -> tuple[dict[str, Any], dict[str, tuple[str, str]]]:
    parsed = parse_body(notification.body)
    prefix = f"notification-{notification.id}:"
    routing: dict[str, tuple[str, str]] = {prefix + "close": ("close", "close")}
    routing.update({prefix + action_id: ("uri", uri) for action_id, uri in parsed.links.items()})

    resources: list[dict[str, Any]] = []
    if app_resource is not None:
        resources.append(app_resource)

    header_children: list[dict[str, Any]] = []
    if app_resource is not None:
        header_children.append(
            {
                "type": "image",
                "resource_id": app_resource["id"],
                "role": "notification-header-icon",
                "accessible_label": notification.app_name or "Application",
            }
        )
    titles = []
    if notification.app_name:
        titles.append(_text(notification.app_name.upper(), "caption"))
    if notification.summary:
        titles.append(_text(notification.summary, "body"))
    if titles:
        header_children.append(
            {"type": "column", "alignment": "start", "gap": "xsmall", "children": titles}
        )
    header_children.extend(
        [
            {"type": "spacer", "flexible": True},
            {
                "type": "action_region",
                "action_id": prefix + "close",
                "accessible_label": "Close notification",
                "content": {
                    "type": "icon",
                    "name": "close",
                    "accessible_label": "Close notification",
                },
            },
        ]
    )
    children: list[dict[str, Any]] = [
        {"type": "row", "gap": "small", "children": header_children}
    ]

    if parsed.items:
        available_inline = inline_resources or {}
        body_items = []
        for item in parsed.items:
            if item["type"] != "inline_image" or item["resource_id"] in available_inline:
                body_item = dict(item)
                if body_item["type"] == "link":
                    body_item["action_id"] = prefix + body_item["action_id"]
                body_items.append(body_item)
        if body_items:
            children.append(
                {"type": "rich_text", "role": "notification-body", "content": body_items}
            )
        resources.extend(available_inline.values())

    buttons = []
    action_index = 0
    has_default = False
    for index in range(0, len(notification.actions), 2):
        key = notification.actions[index]
        action_label = notification.actions[index + 1]
        if key == "default":
            has_default = True
            routing[prefix + "default"] = ("dbus", key)
            continue
        action_id = prefix + f"action-{action_index}"
        action_index += 1
        routing[action_id] = ("dbus", key)
        buttons.append(
            {
                "type": "button",
                "action_id": action_id,
                "accessible_label": action_label,
                "content": _text(action_label, "button"),
            }
        )
    if buttons:
        children.append({"type": "row", "gap": "small", "children": buttons})

    expanded: dict[str, Any] = {
        "type": "column",
        "alignment": "start",
        "gap": "small",
        "children": children,
    }
    if has_default:
        expanded = {
            "type": "action_region",
            "action_id": prefix + "default",
            "accessible_label": "Open notification",
            "content": expanded,
        }

    publication: dict[str, Any] = {
        "type": "publish",
        "context_id": notification.context_id,
        "priority": notification.priority,
        "views": {"expanded": expanded},
        "presentation": {"reveal": "expanded", "duration_ms": 1000},
    }
    if resources:
        publication["resources"] = resources
    return publication, routing
