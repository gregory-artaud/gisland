file(REMOVE_RECURSE "${STAGING_DIR}")
execute_process(
  COMMAND
    "${CMAKE_COMMAND}" -E env "DESTDIR=${STAGING_DIR}" "${CMAKE_COMMAND}" --install "${BUILD_DIR}"
  RESULT_VARIABLE install_result
  OUTPUT_VARIABLE install_output
  ERROR_VARIABLE install_error
)
if(NOT install_result EQUAL 0)
  message(FATAL_ERROR "install failed: ${install_output}${install_error}")
endif()

set(root "${STAGING_DIR}${INSTALL_PREFIX}")
set(required_files
    "${root}/${BINDIR}/gisland"
    "${root}/${BINDIR}/gislandctl"
    "${root}/${BINDIR}/gisland-clock-calendar"
    "${root}/${BINDIR}/gisland-notifications"
    "${root}/${DATADIR}/gisland/distributed/config.toml"
    "${root}/${DATADIR}/gisland/distributed/themes/default.toml"
    "${root}/${DATADIR}/gisland/distributed/modules/clock-calendar/module.toml"
    "${root}/${DATADIR}/gisland/distributed/modules/notifications/module.toml"
    "${root}/${DATADIR}/gisland/notifications/gisland_notifications/application.py"
    "${root}/${DATADIR}/systemd/user/gisland.service")
foreach(required_file IN LISTS required_files)
  if(NOT EXISTS "${required_file}")
    message(FATAL_ERROR "missing installed file: ${required_file}")
  endif()
endforeach()

file(READ "${root}/${DATADIR}/gisland/distributed/modules/clock-calendar/module.toml" manifest)
set(expected_command "command = [\"${INSTALL_PREFIX}/${BINDIR}/gisland-clock-calendar\"]")
string(FIND "${manifest}" "${expected_command}" command_position)
if(command_position EQUAL -1)
  message(FATAL_ERROR "installed manifest has the wrong command: ${manifest}")
endif()

file(READ "${root}/${DATADIR}/gisland/distributed/modules/notifications/module.toml"
     notification_manifest)
set(notification_command "command = [\"${INSTALL_PREFIX}/${BINDIR}/gisland-notifications\"]")
string(FIND "${notification_manifest}" "${notification_command}" notification_command_position)
if(notification_command_position EQUAL -1)
  message(FATAL_ERROR "installed notification manifest has the wrong command: ${notification_manifest}")
endif()
string(FIND "${notification_manifest}" "minimum_minor = 4" notification_minimum_position)
string(FIND "${notification_manifest}" "maximum_minor = 4" notification_maximum_position)
if(notification_minimum_position EQUAL -1 OR notification_maximum_position EQUAL -1)
  message(FATAL_ERROR "installed notification manifest has the wrong protocol: ${notification_manifest}")
endif()
string(FIND "${notification_manifest}" "reveal_duration_ms = 1000" notification_default_position)
string(FIND "${notification_manifest}" "[options_schema.reveal_duration_ms]"
       notification_schema_position)
if(notification_default_position EQUAL -1 OR notification_schema_position EQUAL -1)
  message(FATAL_ERROR "installed notification manifest lacks reveal duration configuration: ${notification_manifest}")
endif()

file(READ "${root}/${BINDIR}/gisland-notifications" notification_executable)
string(FIND "${notification_executable}" "main(\"1.0.0\")" notification_version_position)
if(notification_version_position EQUAL -1)
  message(FATAL_ERROR "installed notification daemon has the wrong project version")
endif()

file(READ "${root}/${DATADIR}/systemd/user/gisland.service" service)
if(NOT service MATCHES "ExecStart=${INSTALL_PREFIX}/${BINDIR}/gisland")
  message(FATAL_ERROR "installed service has the wrong executable: ${service}")
endif()
if(NOT service MATCHES "Restart=on-failure")
  message(FATAL_ERROR "installed service does not restart on failure")
endif()
