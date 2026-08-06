from dataclasses import dataclass
from xml.etree import ElementTree


MAX_ITEMS = 128
MAX_TEXT_BYTES = 16 * 1024
MAX_LINKS = 32


@dataclass(frozen=True)
class ParsedBody:
    items: list[dict]
    links: dict[str, str]
    images: dict[str, tuple[str, str]]


def _plain(value: str) -> ParsedBody:
    return ParsedBody(items=[{"type": "text", "value": value}] if value else [], links={}, images={})


def parse_body(value: str) -> ParsedBody:
    lowered = value.lower()
    if "<!doctype" in lowered or "<!entity" in lowered:
        return _plain(value)
    try:
        root = ElementTree.fromstring(f"<root>{value}</root>")
    except ElementTree.ParseError:
        return _plain(value)

    items: list[dict] = []
    links: dict[str, str] = {}
    images: dict[str, tuple[str, str]] = {}
    text_bytes = 0

    def append_text(text: str | None, emphasis: tuple[str, ...], link_id: str | None) -> None:
        nonlocal text_bytes
        if not text or len(items) >= MAX_ITEMS or text_bytes >= MAX_TEXT_BYTES:
            return
        remaining = MAX_TEXT_BYTES - text_bytes
        encoded = text.encode("utf-8")
        if len(encoded) > remaining:
            encoded = encoded[:remaining]
            while encoded:
                try:
                    text = encoded.decode("utf-8")
                    break
                except UnicodeDecodeError:
                    encoded = encoded[:-1]
            else:
                return
        text_bytes += len(text.encode("utf-8"))
        if link_id is None:
            item = {"type": "text", "value": text}
            if emphasis:
                item["emphasis"] = list(emphasis)
        else:
            item = {
                "type": "link",
                "value": text,
                "emphasis": list(emphasis),
                "action_id": link_id,
                "accessible_label": text,
            }
        items.append(item)

    def walk(element: ElementTree.Element, emphasis: tuple[str, ...], link_id: str | None) -> None:
        append_text(element.text, emphasis, link_id)
        for child in element:
            tag = child.tag.lower() if isinstance(child.tag, str) else ""
            child_emphasis = emphasis
            child_link = link_id
            if tag in ("b", "i", "u"):
                named = {"b": "bold", "i": "italic", "u": "underline"}[tag]
                if named not in child_emphasis:
                    child_emphasis += (named,)
            elif tag == "a" and len(links) < MAX_LINKS:
                href = child.attrib.get("href", "")
                if href:
                    child_link = f"link-{len(links)}"
                    links[child_link] = href
            elif tag == "img" and len(items) < MAX_ITEMS:
                source = child.attrib.get("src", "")
                if source:
                    resource_id = f"inline-{len(images)}"
                    label = child.attrib.get("alt", "")
                    images[resource_id] = (source, label)
                    items.append(
                        {
                            "type": "inline_image",
                            "resource_id": resource_id,
                            "role": "notification-inline-image",
                            "accessible_label": label,
                        }
                    )
            if tag != "img":
                walk(child, child_emphasis, child_link)
            append_text(child.tail, emphasis, link_id)

    walk(root, (), None)
    return ParsedBody(items=items, links=links, images=images)
