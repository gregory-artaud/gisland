from collections.abc import Callable
from typing import Any

from .model import BatteryReading


class UPowerSource:
    def __init__(self, update: Callable[[BatteryReading], None]):
        import gi

        gi.require_version("Gio", "2.0")
        from gi.repository import Gio

        self._Gio = Gio
        self._update = update
        self._root = None
        self._device = None

    def start(self) -> None:
        Gio = self._Gio
        self._root = Gio.DBusProxy.new_for_bus_sync(
            Gio.BusType.SYSTEM,
            Gio.DBusProxyFlags.NONE,
            None,
            "org.freedesktop.UPower",
            "/org/freedesktop/UPower",
            "org.freedesktop.UPower",
            None,
        )
        self._device = Gio.DBusProxy.new_for_bus_sync(
            Gio.BusType.SYSTEM,
            Gio.DBusProxyFlags.NONE,
            None,
            "org.freedesktop.UPower",
            "/org/freedesktop/UPower/devices/DisplayDevice",
            "org.freedesktop.UPower.Device",
            None,
        )
        self._root.connect("g-properties-changed", self._changed)
        self._device.connect("g-properties-changed", self._changed)
        self._emit()

    def _property(self, proxy: Any, name: str, default: Any) -> Any:
        value = proxy.get_cached_property(name)
        return default if value is None else value.unpack()

    def _changed(self, _proxy: Any, _changed: Any, _invalidated: Any) -> None:
        self._emit()

    def _emit(self) -> None:
        state_number = int(self._property(self._device, "State", 0))
        state = {
            1: "charging",
            2: "discharging",
            3: "empty",
            4: "fully-charged",
            5: "pending-charge",
            6: "pending-discharge",
        }.get(state_number, "unknown")
        self._update(
            BatteryReading(
                percentage=float(self._property(self._device, "Percentage", float("nan"))),
                on_battery=bool(self._property(self._root, "OnBattery", state == "discharging")),
                state=state,
                time_to_empty=int(self._property(self._device, "TimeToEmpty", 0)),
                time_to_full=int(self._property(self._device, "TimeToFull", 0)),
                energy_rate=float(self._property(self._device, "EnergyRate", 0.0)),
                energy_full=float(self._property(self._device, "EnergyFull", 0.0)),
                energy_full_design=float(self._property(self._device, "EnergyFullDesign", 0.0)),
                present=bool(self._property(self._device, "IsPresent", False)),
            )
        )
