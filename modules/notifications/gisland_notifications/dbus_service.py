from typing import Any

from .model import CloseReason


BUS_NAME = "org.freedesktop.Notifications"
OBJECT_PATH = "/org/freedesktop/Notifications"
INTERFACE_NAME = BUS_NAME
HISTORY_OBJECT_PATH = "/org/gisland/Notifications/History"
HISTORY_INTERFACE_NAME = "org.gisland.Notifications.History"

INTROSPECTION_XML = """
<node>
  <interface name="org.freedesktop.Notifications">
    <method name="GetCapabilities">
      <arg direction="out" name="capabilities" type="as"/>
    </method>
    <method name="Notify">
      <arg direction="in" name="app_name" type="s"/>
      <arg direction="in" name="replaces_id" type="u"/>
      <arg direction="in" name="app_icon" type="s"/>
      <arg direction="in" name="summary" type="s"/>
      <arg direction="in" name="body" type="s"/>
      <arg direction="in" name="actions" type="as"/>
      <arg direction="in" name="hints" type="a{sv}"/>
      <arg direction="in" name="expire_timeout" type="i"/>
      <arg direction="out" name="id" type="u"/>
    </method>
    <method name="CloseNotification">
      <arg direction="in" name="id" type="u"/>
    </method>
    <method name="GetServerInformation">
      <arg direction="out" name="name" type="s"/>
      <arg direction="out" name="vendor" type="s"/>
      <arg direction="out" name="version" type="s"/>
      <arg direction="out" name="spec_version" type="s"/>
    </method>
    <signal name="NotificationClosed">
      <arg name="id" type="u"/>
      <arg name="reason" type="u"/>
    </signal>
    <signal name="ActionInvoked">
      <arg name="id" type="u"/>
      <arg name="action_key" type="s"/>
    </signal>
  </interface>
</node>
"""

HISTORY_INTROSPECTION_XML = """
<node>
  <interface name="org.gisland.Notifications.History">
    <method name="ShowMore">
      <arg direction="out" name="visible_count" type="u"/>
    </method>
    <method name="ConfirmVisible"/>
  </interface>
</node>
"""

CAPABILITIES = [
    "actions",
    "body",
    "body-markup",
    "body-hyperlinks",
    "body-images",
    "icon-static",
    "persistence",
]


def _unpack(value: Any) -> Any:
    if hasattr(value, "unpack"):
        return _unpack(value.unpack())
    if isinstance(value, dict):
        return {key: _unpack(item) for key, item in value.items()}
    if isinstance(value, (tuple, list)):
        return type(value)(_unpack(item) for item in value)
    return value


class NotificationDBusService:
    def __init__(self, ready, fatal, version: str = "1"):
        self._ready = ready
        self._fatal = fatal
        self._version = version
        self._service = None
        self._connection = None
        self._owner_id = 0
        self._registration_id = 0
        self._history_registration_id = 0
        self._stopping = False

    def set_service(self, service) -> None:
        self._service = service

    def start(self) -> None:
        import gi

        gi.require_version("Gio", "2.0")
        from gi.repository import Gio

        self._owner_id = Gio.bus_own_name(
            Gio.BusType.SESSION,
            BUS_NAME,
            Gio.BusNameOwnerFlags.DO_NOT_QUEUE,
            self._bus_acquired,
            self._name_acquired,
            self._name_lost,
        )

    def _bus_acquired(self, connection, _name) -> None:
        from gi.repository import Gio

        self._connection = connection
        node = Gio.DBusNodeInfo.new_for_xml(INTROSPECTION_XML)
        self._registration_id = connection.register_object(
            OBJECT_PATH, node.interfaces[0], self._method_call, None, None
        )
        history_node = Gio.DBusNodeInfo.new_for_xml(HISTORY_INTROSPECTION_XML)
        self._history_registration_id = connection.register_object(
            HISTORY_OBJECT_PATH,
            history_node.interfaces[0],
            self._history_method_call,
            None,
            None,
        )

    def _name_acquired(self, _connection, _name) -> None:
        self._ready()

    def _name_lost(self, _connection, _name) -> None:
        if not self._stopping:
            self._fatal("could not own org.freedesktop.Notifications")

    def _method_call(
        self,
        _connection,
        _sender,
        _object_path,
        _interface_name,
        method_name,
        parameters,
        invocation,
    ) -> None:
        from gi.repository import GLib

        if self._service is None:
            invocation.return_dbus_error("org.freedesktop.DBus.Error.Failed", "service unavailable")
            return
        try:
            values = _unpack(parameters)
            if method_name == "GetCapabilities":
                invocation.return_value(GLib.Variant("(as)", (CAPABILITIES,)))
            elif method_name == "GetServerInformation":
                invocation.return_value(
                    GLib.Variant("(ssss)", ("gisland", "gisland", self._version, "1.2"))
                )
            elif method_name == "Notify":
                app_name, replaces_id, app_icon, summary, body, actions, hints, timeout = values
                notification_id = self._service.notify(
                    app_name=str(app_name),
                    replaces_id=int(replaces_id),
                    app_icon=str(app_icon),
                    summary=str(summary),
                    body=str(body),
                    actions=tuple(actions),
                    hints=dict(hints),
                    expire_timeout=int(timeout),
                )
                invocation.return_value(GLib.Variant("(u)", (notification_id,)))
            elif method_name == "CloseNotification":
                self._service.close(int(values[0]), CloseReason.CLOSED_BY_CALL)
                invocation.return_value(None)
            else:
                invocation.return_dbus_error(
                    "org.freedesktop.DBus.Error.UnknownMethod", f"unknown method: {method_name}"
                )
        except (TypeError, ValueError) as error:
            invocation.return_dbus_error("org.freedesktop.DBus.Error.InvalidArgs", str(error))
        except Exception as error:
            invocation.return_dbus_error("org.freedesktop.DBus.Error.Failed", str(error))

    def _history_method_call(
        self,
        _connection,
        _sender,
        _object_path,
        _interface_name,
        method_name,
        _parameters,
        invocation,
    ) -> None:
        from gi.repository import GLib

        if self._service is None:
            invocation.return_dbus_error("org.freedesktop.DBus.Error.Failed", "service unavailable")
            return
        if method_name not in ("ShowMore", "ConfirmVisible"):
            invocation.return_dbus_error(
                "org.freedesktop.DBus.Error.UnknownMethod", f"unknown method: {method_name}"
            )
            return
        try:
            if method_name == "ShowMore":
                invocation.return_value(GLib.Variant("(u)", (self._service.show_more(),)))
            else:
                self._service.history_opened()
                invocation.return_value(None)
        except (TypeError, ValueError) as error:
            invocation.return_dbus_error("org.freedesktop.DBus.Error.InvalidArgs", str(error))
        except Exception as error:
            invocation.return_dbus_error("org.freedesktop.DBus.Error.Failed", str(error))

    def emit_signal(self, name: str, notification_id: int, value: Any) -> None:
        if self._connection is None:
            return
        from gi.repository import GLib

        if name == "ActionInvoked":
            parameters = GLib.Variant("(us)", (notification_id, str(value)))
        else:
            parameters = GLib.Variant("(uu)", (notification_id, int(value)))
        self._connection.emit_signal(None, OBJECT_PATH, INTERFACE_NAME, name, parameters)

    def stop(self) -> None:
        self._stopping = True
        if self._connection is not None and self._history_registration_id:
            self._connection.unregister_object(self._history_registration_id)
            self._history_registration_id = 0
        if self._connection is not None and self._registration_id:
            self._connection.unregister_object(self._registration_id)
            self._registration_id = 0
        if self._owner_id:
            from gi.repository import Gio

            Gio.bus_unown_name(self._owner_id)
            self._owner_id = 0
        self._connection = None
