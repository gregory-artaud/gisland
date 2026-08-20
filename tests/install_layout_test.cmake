file(REMOVE_RECURSE "${STAGING_DIR}")
set(root "${STAGING_DIR}${INSTALL_PREFIX}")
set(personal_manifest "${STAGING_DIR}/personal/gisland/modules/existing/module.toml")
set(prefix_sentinel "${root}/${DATADIR}/gisland/custom/sentinel")
file(MAKE_DIRECTORY "${STAGING_DIR}/personal/gisland/modules/existing")
file(MAKE_DIRECTORY "${root}/${DATADIR}/gisland/custom")
file(WRITE "${personal_manifest}" "personal module sentinel\n")
file(WRITE "${prefix_sentinel}" "custom prefix sentinel\n")
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

set(required_files
    "${root}/${BINDIR}/gisland"
    "${root}/${BINDIR}/gislandctl"
    "${root}/${BINDIR}/gisland-lua-host"
    "${root}/${DATADIR}/gisland/distributed/config.toml"
    "${root}/${DATADIR}/gisland/distributed/themes/default.toml"
    "${root}/${DATADIR}/gisland/distributed/modules/clock-calendar/module.toml"
    "${root}/${DATADIR}/gisland/distributed/modules/clock-calendar/config.toml"
    "${root}/${DATADIR}/gisland/distributed/modules/clock-calendar/view.toml"
    "${root}/${DATADIR}/gisland/distributed/modules/clock-calendar/clock_calendar.lua"
    "${root}/${DATADIR}/gisland/distributed/modules/notifications/module.toml"
    "${root}/${DATADIR}/gisland/distributed/modules/notifications/notifications.lua"
    "${root}/${DATADIR}/gisland/distributed/modules/battery/module.toml"
    "${root}/${DATADIR}/gisland/distributed/modules/battery/config.toml"
    "${root}/${DATADIR}/gisland/distributed/modules/battery/view.toml"
    "${root}/${DATADIR}/gisland/distributed/modules/battery/battery.lua"
    "${root}/${DATADIR}/gisland/distributed/modules/audio/module.toml"
    "${root}/${DATADIR}/gisland/distributed/modules/lua-example/module.toml"
    "${root}/${DATADIR}/gisland/distributed/modules/lua-example/config.toml"
    "${root}/${DATADIR}/gisland/distributed/modules/lua-example/view.toml"
    "${root}/${DATADIR}/gisland/distributed/modules/lua-example/example.lua"
    "${root}/${DATADIR}/gisland/distributed/modules/audio/config.toml"
    "${root}/${DATADIR}/gisland/distributed/modules/audio/audio.lua"
    "${root}/${DATADIR}/gisland/distributed/modules/audio/command.lua"
    "${root}/${DATADIR}/systemd/user/gisland.service")
foreach(required_file IN LISTS required_files)
  if(NOT EXISTS "${required_file}")
    message(FATAL_ERROR "missing installed file: ${required_file}")
  endif()
endforeach()

file(GLOB installed_clock_lua_files LIST_DIRECTORIES false
     "${root}/${DATADIR}/gisland/distributed/modules/clock-calendar/*.lua")
set(expected_clock_lua_file
    "${root}/${DATADIR}/gisland/distributed/modules/clock-calendar/clock_calendar.lua")
if(NOT installed_clock_lua_files STREQUAL expected_clock_lua_file)
  message(FATAL_ERROR
          "clock-calendar must install only its entry Lua file: ${installed_clock_lua_files}")
endif()

set(forbidden_paths
    "${root}/${BINDIR}/gisland-clock-calendar"
    "${root}/${DATADIR}/gisland/distributed/modules/clock-calendar/calendar.lua"
    "${root}/${BINDIR}/gisland-audio"
    "${root}/${BINDIR}/gisland-audio-control"
    "${root}/${DATADIR}/gisland/audio"
    "${root}/${DATADIR}/gisland/distributed/modules/audio-lua"
    "${root}/${BINDIR}/gisland-battery"
    "${root}/${DATADIR}/gisland/battery"
    "${root}/${BINDIR}/gisland-notifications"
    "${root}/${BINDIR}/gisland-notification-history"
    "${root}/${DATADIR}/gisland/notifications"
    "${root}/${DATADIR}/dbus-1/services/org.gisland.Audio.service"
    "${root}/${DATADIR}/systemd/user/gisland-audio.service")
foreach(forbidden_path IN LISTS forbidden_paths)
  if(EXISTS "${forbidden_path}")
    message(FATAL_ERROR "forbidden path was installed: ${forbidden_path}")
  endif()
endforeach()

file(READ "${personal_manifest}" personal_manifest_contents)
if(NOT personal_manifest_contents STREQUAL "personal module sentinel\n")
  message(FATAL_ERROR "installation modified a personal module")
endif()

file(GLOB installed_battery_lua_files LIST_DIRECTORIES false
     "${root}/${DATADIR}/gisland/distributed/modules/battery/*.lua")
set(expected_battery_lua_file
    "${root}/${DATADIR}/gisland/distributed/modules/battery/battery.lua")
if(NOT installed_battery_lua_files STREQUAL expected_battery_lua_file)
  message(FATAL_ERROR
          "battery must install only its entry Lua file: ${installed_battery_lua_files}")
endif()
file(READ "${prefix_sentinel}" prefix_sentinel_contents)
if(NOT prefix_sentinel_contents STREQUAL "custom prefix sentinel\n")
  message(FATAL_ERROR "installation modified an unrelated custom-prefix file")
endif()

file(READ "${root}/${DATADIR}/gisland/distributed/config.toml" distributed_config)
string(FIND "${distributed_config}" "module = \"lua-example\"" example_enabled_position)
if(NOT example_enabled_position EQUAL -1)
  message(FATAL_ERROR "the distributed Lua example must not be enabled by default")
endif()
file(READ "${root}/${DATADIR}/gisland/distributed/modules/lua-example/module.toml"
     example_manifest)
set(example_command "command = [\"${INSTALL_PREFIX}/${BINDIR}/gisland-lua-host\"]")
string(FIND "${example_manifest}" "${example_command}" example_command_position)
string(FIND "${example_manifest}" "entry = \"example.lua\"" example_entry_position)
string(FIND "${example_manifest}" "config = \"config.toml\"" example_config_position)
string(FIND "${example_manifest}" "view = \"view.toml\"" example_view_position)
if(example_command_position EQUAL -1 OR example_entry_position EQUAL -1 OR
   example_config_position EQUAL -1 OR example_view_position EQUAL -1)
  message(FATAL_ERROR "installed Lua example manifest is incomplete: ${example_manifest}")
endif()

file(READ "${root}/${DATADIR}/gisland/distributed/modules/clock-calendar/module.toml" manifest)
set(expected_command "command = [\"${INSTALL_PREFIX}/${BINDIR}/gisland-lua-host\"]")
string(FIND "${manifest}" "${expected_command}" command_position)
string(FIND "${manifest}" "entry = \"clock_calendar.lua\"" entry_position)
string(FIND "${manifest}" "config = \"config.toml\"" config_position)
string(FIND "${manifest}" "view = \"view.toml\"" view_position)
string(FIND "${manifest}" "minimum_minor = 8" minimum_position)
string(FIND "${manifest}" "maximum_minor = 9" maximum_position)
string(FIND "${manifest}" "[defaults]" inline_defaults_position)
if(command_position EQUAL -1 OR entry_position EQUAL -1 OR config_position EQUAL -1 OR
   view_position EQUAL -1 OR minimum_position EQUAL -1 OR maximum_position EQUAL -1 OR
   NOT inline_defaults_position EQUAL -1)
  message(FATAL_ERROR "installed clock-calendar manifest is incomplete: ${manifest}")
endif()

file(READ "${root}/${DATADIR}/gisland/distributed/modules/audio/module.toml" audio_manifest)
set(audio_command "command = [\"${INSTALL_PREFIX}/${BINDIR}/gisland-lua-host\"]")
string(FIND "${audio_manifest}" "${audio_command}" audio_command_position)
string(FIND "${audio_manifest}" "entry = \"audio.lua\"" audio_entry_position)
string(FIND "${audio_manifest}" "config = \"config.toml\"" audio_config_position)
string(FIND "${audio_manifest}" "minimum_minor = 8" audio_minimum_position)
string(FIND "${audio_manifest}" "maximum_minor = 8" audio_maximum_position)
string(FIND "${audio_manifest}" "minimum = 1" audio_step_minimum_position)
string(FIND "${audio_manifest}" "maximum = 60000" audio_duration_maximum_position)
string(FIND "${audio_manifest}" "[defaults]" audio_inline_defaults_position)
if(audio_command_position EQUAL -1 OR audio_entry_position EQUAL -1 OR
   audio_config_position EQUAL -1 OR audio_minimum_position EQUAL -1 OR
   audio_maximum_position EQUAL -1 OR audio_step_minimum_position EQUAL -1 OR
   audio_duration_maximum_position EQUAL -1 OR NOT audio_inline_defaults_position EQUAL -1)
  message(FATAL_ERROR "installed audio manifest is incomplete: ${audio_manifest}")
endif()

file(READ "${root}/${DATADIR}/gisland/distributed/modules/battery/module.toml"
     battery_manifest)
set(battery_command "command = [\"${INSTALL_PREFIX}/${BINDIR}/gisland-lua-host\"]")
string(FIND "${battery_manifest}" "${battery_command}" battery_command_position)
string(FIND "${battery_manifest}" "entry = \"battery.lua\"" battery_entry_position)
string(FIND "${battery_manifest}" "config = \"config.toml\"" battery_config_position)
string(FIND "${battery_manifest}" "view = \"view.toml\"" battery_view_position)
string(FIND "${battery_manifest}" "minimum_minor = 8" battery_minimum_position)
string(FIND "${battery_manifest}" "maximum_minor = 8" battery_maximum_position)
string(FIND "${battery_manifest}" "maximum = 60000" battery_duration_maximum_position)
string(FIND "${battery_manifest}" "[defaults]" battery_inline_defaults_position)
if(battery_command_position EQUAL -1 OR battery_entry_position EQUAL -1 OR
   battery_config_position EQUAL -1 OR battery_view_position EQUAL -1 OR
   battery_minimum_position EQUAL -1 OR battery_maximum_position EQUAL -1 OR
   battery_duration_maximum_position EQUAL -1 OR
   NOT battery_inline_defaults_position EQUAL -1)
  message(FATAL_ERROR "installed battery manifest is incomplete: ${battery_manifest}")
endif()

file(READ "${root}/${DATADIR}/gisland/distributed/modules/notifications/module.toml"
     notification_manifest)
set(notification_command "command = [\"${INSTALL_PREFIX}/${BINDIR}/gisland-lua-host\"]")
string(FIND "${notification_manifest}" "${notification_command}" notification_command_position)
string(FIND "${notification_manifest}" "entry = \"notifications.lua\"" notification_entry_position)
if(notification_command_position EQUAL -1 OR notification_entry_position EQUAL -1)
  message(FATAL_ERROR "installed notification manifest has the wrong command: ${notification_manifest}")
endif()
string(FIND "${notification_manifest}" "minimum_minor = 8" notification_minimum_position)
string(FIND "${notification_manifest}" "maximum_minor = 8" notification_maximum_position)
if(notification_minimum_position EQUAL -1 OR notification_maximum_position EQUAL -1)
  message(FATAL_ERROR "installed notification manifest has the wrong protocol: ${notification_manifest}")
endif()
string(FIND "${notification_manifest}" "reveal_duration_ms = 1000" notification_default_position)
string(FIND "${notification_manifest}" "[options_schema.reveal_duration_ms]"
       notification_schema_position)
string(FIND "${notification_manifest}" "history_limit = 100" history_limit_position)
string(FIND "${notification_manifest}" "history_visible_limit = 5" history_visible_position)
if(notification_default_position EQUAL -1 OR notification_schema_position EQUAL -1 OR
   history_limit_position EQUAL -1 OR history_visible_position EQUAL -1)
  message(FATAL_ERROR "installed notification manifest lacks reveal duration configuration: ${notification_manifest}")
endif()

file(GLOB installed_notification_lua_files LIST_DIRECTORIES false
     "${root}/${DATADIR}/gisland/distributed/modules/notifications/*.lua")
set(expected_notification_lua_file
    "${root}/${DATADIR}/gisland/distributed/modules/notifications/notifications.lua")
if(NOT installed_notification_lua_files STREQUAL expected_notification_lua_file)
  message(FATAL_ERROR
          "notifications must install only its entry Lua file: ${installed_notification_lua_files}")
endif()

file(READ "${root}/${DATADIR}/systemd/user/gisland.service" service)
if(NOT service MATCHES "ExecStart=${INSTALL_PREFIX}/${BINDIR}/gisland")
  message(FATAL_ERROR "installed service has the wrong executable: ${service}")
endif()
if(NOT service MATCHES "Restart=on-failure")
  message(FATAL_ERROR "installed service does not restart on failure")
endif()

set(upgrade_staging_dir "${STAGING_DIR}-manual-upgrade")
set(upgrade_root "${upgrade_staging_dir}${INSTALL_PREFIX}")
set(upgrade_legacy_wrapper "${upgrade_root}/${BINDIR}/gisland-audio")
set(upgrade_legacy_clock "${upgrade_root}/${BINDIR}/gisland-clock-calendar")
set(upgrade_legacy_battery "${upgrade_root}/${BINDIR}/gisland-battery")
set(upgrade_sentinel "${upgrade_root}/${DATADIR}/gisland/custom/sentinel")
file(REMOVE_RECURSE "${upgrade_staging_dir}")
file(MAKE_DIRECTORY "${upgrade_root}/${BINDIR}" "${upgrade_root}/${DATADIR}/gisland/custom")
file(WRITE "${upgrade_legacy_wrapper}" "legacy wrapper sentinel\n")
file(WRITE "${upgrade_legacy_clock}" "legacy clock sentinel\n")
file(WRITE "${upgrade_legacy_battery}" "legacy battery sentinel\n")
file(WRITE "${upgrade_sentinel}" "manual upgrade sentinel\n")
execute_process(
  COMMAND
    "${CMAKE_COMMAND}" -E env "DESTDIR=${upgrade_staging_dir}" "${CMAKE_COMMAND}" --install
    "${BUILD_DIR}"
  RESULT_VARIABLE upgrade_install_result
  OUTPUT_VARIABLE upgrade_install_output
  ERROR_VARIABLE upgrade_install_error
)
if(NOT upgrade_install_result EQUAL 0)
  message(FATAL_ERROR
          "manual upgrade install failed: ${upgrade_install_output}${upgrade_install_error}")
endif()
file(READ "${upgrade_legacy_wrapper}" upgrade_legacy_contents)
if(NOT upgrade_legacy_contents STREQUAL "legacy wrapper sentinel\n")
  message(FATAL_ERROR "manual CMake install unexpectedly changed a stale legacy file")
endif()
file(READ "${upgrade_legacy_clock}" upgrade_legacy_clock_contents)
if(NOT upgrade_legacy_clock_contents STREQUAL "legacy clock sentinel\n")
  message(FATAL_ERROR "manual CMake install unexpectedly changed the stale native clock")
endif()
file(READ "${upgrade_legacy_battery}" upgrade_legacy_battery_contents)
if(NOT upgrade_legacy_battery_contents STREQUAL "legacy battery sentinel\n")
  message(FATAL_ERROR "manual CMake install unexpectedly changed the stale battery wrapper")
endif()
file(READ "${upgrade_sentinel}" upgrade_sentinel_contents)
if(NOT upgrade_sentinel_contents STREQUAL "manual upgrade sentinel\n")
  message(FATAL_ERROR "manual CMake install modified an unrelated custom-prefix file")
endif()
