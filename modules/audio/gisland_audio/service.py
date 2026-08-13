from collections.abc import Callable
from typing import Any, Protocol

from .model import AudioOptions, AudioState


class Mixer(Protocol):
    def read_state(self) -> AudioState: ...

    def set_volume(self, percent: int) -> None: ...

    def set_muted(self, muted: bool) -> None: ...


class AudioService:
    def __init__(
        self,
        write_record: Callable[[dict[str, Any]], None],
        mixer: Mixer,
        close_expanded: Callable[[], None],
    ):
        self._write_record = write_record
        self._mixer = mixer
        self._close_expanded = close_expanded
        self._options = AudioOptions()
        self._configured = False
        self._active_context: str | None = None

    def configure(self, values: dict[str, Any]) -> None:
        self._options = AudioOptions.from_mapping(values)
        self._configured = True

    def ready(self) -> None:
        pass

    def action(self, _action_id: str) -> bool:
        return False

    def toggle_mute(self) -> None:
        self._require_configured()
        before = self._mixer.read_state()
        self._mixer.set_muted(not before.muted)
        after = self._mixer.read_state()
        self._publish_mute(after)

    def change_volume(self, delta_percent: int) -> None:
        self._require_configured()
        before = self._mixer.read_state()
        target = max(0, min(before.volume_percent + delta_percent, self._options.maximum_percent))
        if before.muted:
            self._mixer.set_muted(False)
        self._mixer.set_volume(target)
        after = self._mixer.read_state()
        self._publish_volume(before, after)

    def _require_configured(self) -> None:
        if not self._configured:
            raise RuntimeError("audio module is not configured")

    def _replace(self, context_id: str, compact: dict[str, Any], style: str) -> None:
        if self._active_context is not None and self._active_context != context_id:
            self._write_record({"type": "dismiss", "context_id": self._active_context})
        self._write_record(
            {
                "type": "publish",
                "context_id": context_id,
                "priority": 80,
                "expires_in_ms": self._options.hud_duration_ms,
                "views": {"compact": compact},
                "presentation": {"compact_style": style},
            }
        )
        self._active_context = context_id
        self._close_expanded()

    def _publish_mute(self, state: AudioState) -> None:
        icon = "volume-muted" if state.muted else "volume-high"
        self._replace(
            "audio-mute",
            {
                "type": "icon",
                "name": icon,
                "role": "hud-mute-icon",
                "accessible_label": "Muted" if state.muted else "Unmuted",
            },
            "hud-symbol",
        )

    def _publish_volume(self, before: AudioState, after: AudioState) -> None:
        maximum = self._options.maximum_percent
        if after.muted or after.volume_percent == 0:
            icon = "volume-muted"
        elif after.volume_percent <= maximum / 2:
            icon = "volume-low"
        else:
            icon = "volume-high"
        self._replace(
            "audio-volume",
            {
                "type": "row",
                "gap": "small",
                "children": [
                    {
                        "type": "icon",
                        "name": icon,
                        "role": "hud-volume-icon",
                        "accessible_label": f"Volume {after.volume_percent} percent",
                    },
                    {
                        "type": "progress",
                        "value": max(0.0, min(after.volume_percent / maximum, 1.0)),
                        "transition_from": max(
                            0.0, min(before.volume_percent / maximum, 1.0)
                        ),
                        "state": "foreground",
                    },
                ],
            },
            "hud-meter",
        )
