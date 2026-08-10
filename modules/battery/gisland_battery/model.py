import json
import os
import tempfile
from dataclasses import dataclass, field
from enum import Enum
from pathlib import Path
from typing import Any


@dataclass(frozen=True)
class BatteryOptions:
    warning_percent: int = 20
    persistent_percent: int = 10
    critical_percent: int = 5
    yellow_percent: int = 30
    red_percent: int = 15
    preview_duration_ms: int = 3000

    @classmethod
    def from_mapping(cls, values: dict[str, Any]) -> "BatteryOptions":
        known = {field.name for field in cls.__dataclass_fields__.values()}
        unknown = set(values) - known
        if unknown:
            raise ValueError(f"unknown battery option: {sorted(unknown)[0]}")
        for name, value in values.items():
            if type(value) is not int:
                raise ValueError(f"{name} must be an integer")
        options = cls(**values)
        if not (
            0
            < options.critical_percent
            <= options.persistent_percent
            <= options.warning_percent
            <= 100
        ):
            raise ValueError(
                "critical_percent, persistent_percent and warning_percent must be ordered"
            )
        if not 0 < options.red_percent <= options.yellow_percent <= 100:
            raise ValueError("red_percent and yellow_percent must be ordered")
        if not 0 <= options.preview_duration_ms <= 60000:
            raise ValueError("preview_duration_ms must be between 0 and 60000")
        return options


@dataclass(frozen=True)
class BatteryReading:
    percentage: float
    on_battery: bool
    state: str
    time_to_empty: int = 0
    time_to_full: int = 0
    energy_rate: float = 0.0
    energy_full: float = 0.0
    energy_full_design: float = 0.0
    present: bool = True


class AlertKind(str, Enum):
    unplugged = "unplugged"
    plugged = "plugged"
    warning = "warning"
    persistent = "persistent"
    critical = "critical"


def _duration(seconds: int) -> str | None:
    if seconds <= 0:
        return None
    minutes = (seconds + 30) // 60
    hours, remainder = divmod(minutes, 60)
    if hours > 0:
        return f"{hours} h {remainder:02d}"
    return f"{remainder} min"


def snapshot_for(reading: BatteryReading, options: BatteryOptions) -> dict[str, Any]:
    percentage = max(0.0, min(reading.percentage, 100.0))
    if percentage > options.yellow_percent:
        semantic_state = "success"
    elif percentage > options.red_percent:
        semantic_state = "warning"
    else:
        semantic_state = "error"
    estimate = _duration(reading.time_to_empty if reading.on_battery else reading.time_to_full)
    if reading.state == "charging":
        state_text = "Charge"
    elif reading.state == "fully-charged":
        state_text = "Chargée"
    elif reading.on_battery:
        state_text = "Décharge"
    else:
        state_text = "Secteur"
    if estimate is None:
        compact_estimate = "Calcul…"
        detail_estimate = "—"
    elif reading.on_battery:
        compact_estimate = estimate
        detail_estimate = f"{estimate} restantes"
    else:
        compact_estimate = estimate
        detail_estimate = f"{estimate} avant charge complète"
    health = "—"
    if reading.energy_full > 0.0 and reading.energy_full_design > 0.0:
        health = f"{round((reading.energy_full / reading.energy_full_design) * 100.0)} %"
    power = "—" if reading.energy_rate <= 0.0 else f"{reading.energy_rate:.1f} W".replace(".", ",")
    return {
        "level": round(percentage / 100.0, 4),
        "percent_text": f"{round(percentage)} %",
        "semantic_state": semantic_state,
        "estimate_compact": compact_estimate,
        "state_text": state_text,
        "estimate_detail": detail_estimate,
        "health_text": health,
        "power_text": power,
    }


@dataclass
class AlertState:
    options: BatteryOptions
    emitted: set[int] = field(default_factory=set)
    previous_on_battery: bool | None = None

    def observe(self, reading: BatteryReading) -> list[AlertKind]:
        result: list[AlertKind] = []
        first = self.previous_on_battery is None
        if not first and reading.on_battery != self.previous_on_battery:
            if reading.on_battery:
                self.emitted.clear()
                result.append(AlertKind.unplugged)
            else:
                result.append(AlertKind.plugged)
        self.previous_on_battery = reading.on_battery
        if not reading.on_battery:
            return result

        crossed: list[tuple[int, AlertKind]] = []
        for threshold, kind in (
            (self.options.warning_percent, AlertKind.warning),
            (self.options.persistent_percent, AlertKind.persistent),
            (self.options.critical_percent, AlertKind.critical),
        ):
            if reading.percentage <= threshold and threshold not in self.emitted:
                self.emitted.add(threshold)
                crossed.append((threshold, kind))
        if crossed:
            result.append(crossed[-1][1])
        return result

    def to_json(self) -> dict[str, Any]:
        return {
            "version": 1,
            "emitted": sorted(self.emitted),
            "previous_on_battery": self.previous_on_battery,
        }


class CycleStateStore:
    def __init__(self, path: Path):
        self._path = path

    def load(self, options: BatteryOptions) -> AlertState:
        try:
            value = json.loads(self._path.read_text(encoding="utf-8"))
            if value.get("version") != 1 or not isinstance(value.get("emitted"), list):
                raise ValueError("unsupported battery cycle state")
            emitted = {int(item) for item in value["emitted"]}
            previous = value.get("previous_on_battery")
            if previous is not None and type(previous) is not bool:
                raise ValueError("invalid power state")
            return AlertState(options, emitted, previous)
        except (OSError, ValueError, TypeError, json.JSONDecodeError):
            return AlertState(options)

    def save(self, state: AlertState) -> None:
        self._path.parent.mkdir(parents=True, exist_ok=True)
        descriptor, temporary = tempfile.mkstemp(prefix=".battery-cycle-", dir=self._path.parent)
        try:
            with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
                json.dump(state.to_json(), stream, separators=(",", ":"))
                stream.write("\n")
                stream.flush()
                os.fsync(stream.fileno())
            os.chmod(temporary, 0o600)
            os.replace(temporary, self._path)
        finally:
            try:
                os.unlink(temporary)
            except FileNotFoundError:
                pass
