from dataclasses import dataclass, fields
from typing import Any


@dataclass(frozen=True)
class AudioOptions:
    step_percent: int = 5
    maximum_percent: int = 150
    hud_duration_ms: int = 1500

    @classmethod
    def from_mapping(cls, values: dict[str, Any]) -> "AudioOptions":
        known = {field.name for field in fields(cls)}
        unknown = set(values) - known
        if unknown:
            raise ValueError(f"unknown audio option: {sorted(unknown)[0]}")
        for name, value in values.items():
            if type(value) is not int:
                raise ValueError(f"{name} must be an integer")
        options = cls(**values)
        if not 1 <= options.step_percent <= 25:
            raise ValueError("step_percent must be between 1 and 25")
        if not 100 <= options.maximum_percent <= 200:
            raise ValueError("maximum_percent must be between 100 and 200")
        if not 100 <= options.hud_duration_ms <= 60000:
            raise ValueError("hud_duration_ms must be between 100 and 60000")
        return options


@dataclass(frozen=True)
class AudioState:
    volume_percent: int
    muted: bool
