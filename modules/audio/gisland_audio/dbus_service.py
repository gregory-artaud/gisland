BUS_NAME = "org.gisland.Audio"
OBJECT_PATH = "/org/gisland/Audio/Controls"
INTERFACE_NAME = "org.gisland.Audio.Controls"

INTROSPECTION_XML = """
<node>
  <interface name="org.gisland.Audio.Controls">
    <method name="ToggleMute"/>
    <method name="ChangeVolume">
      <arg direction="in" name="delta_percent" type="i"/>
    </method>
  </interface>
</node>
"""


class AudioDBusService:
    def __init__(self, ready, fatal):
        self._ready = ready
        self._fatal = fatal
        self._service = None
        self._connection = None
        self._owner_id = 0
        self._registration_id = 0
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

    def _name_acquired(self, _connection, _name) -> None:
        self._ready()

    def _name_lost(self, _connection, _name) -> None:
        if not self._stopping:
            self._fatal(f"could not own {BUS_NAME}")

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
        if self._service is None:
            invocation.return_dbus_error("org.freedesktop.DBus.Error.Failed", "service unavailable")
            return
        try:
            if method_name == "ToggleMute":
                self._service.toggle_mute()
            elif method_name == "ChangeVolume":
                values = parameters.unpack()
                if len(values) != 1 or type(values[0]) is not int or values[0] == 0:
                    raise ValueError("delta_percent must be a non-zero integer")
                self._service.change_volume(values[0])
            else:
                invocation.return_dbus_error(
                    "org.freedesktop.DBus.Error.UnknownMethod", f"unknown method: {method_name}"
                )
                return
            invocation.return_value(None)
        except (TypeError, ValueError) as error:
            invocation.return_dbus_error("org.freedesktop.DBus.Error.InvalidArgs", str(error))
        except Exception as error:
            invocation.return_dbus_error("org.freedesktop.DBus.Error.Failed", str(error))

    def stop(self) -> None:
        self._stopping = True
        if self._connection is not None and self._registration_id:
            self._connection.unregister_object(self._registration_id)
            self._registration_id = 0
        if self._owner_id:
            from gi.repository import Gio

            Gio.bus_unown_name(self._owner_id)
            self._owner_id = 0
        self._connection = None
