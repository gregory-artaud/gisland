cmake_minimum_required(VERSION 3.28)

project(gisland VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "gisland currently supports Linux/X11 only")
endif()

include(CTest)
include(FetchContent)
include(GNUInstallDirs)
include(cmake/ProjectOptions.cmake)

option(GISLAND_BUILD_GRAPHICAL_TESTS "Build tests that require an X11/OpenGL display" OFF)
set(GISLAND_LGI_ROOT "" CACHE PATH "Optional root containing a Lua 5.4-compatible lgi")

set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_X11 ON CACHE BOOL "" FORCE)
set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
set(PLATFORM Desktop CACHE STRING "" FORCE)

FetchContent_Declare(
  raylib
  GIT_REPOSITORY https://github.com/raysan5/raylib.git
  GIT_TAG 6.0
  GIT_SHALLOW TRUE
  SYSTEM
  EXCLUDE_FROM_ALL
)
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.12.0
  GIT_SHALLOW TRUE
  SYSTEM
  EXCLUDE_FROM_ALL
)
FetchContent_Declare(
  tomlplusplus
  GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
  GIT_TAG v3.4.0
  GIT_SHALLOW TRUE
  SYSTEM
  EXCLUDE_FROM_ALL
)
if(BUILD_TESTING)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.8.1
    GIT_SHALLOW TRUE
    SYSTEM
    EXCLUDE_FROM_ALL
  )
endif()

FetchContent_MakeAvailable(raylib nlohmann_json tomlplusplus)
if(BUILD_TESTING)
  FetchContent_MakeAvailable(Catch2)
endif()
set(GISLAND_X11_COMPONENTS Xext Xrandr)
if(BUILD_TESTING AND GISLAND_BUILD_GRAPHICAL_TESTS)
  list(APPEND GISLAND_X11_COMPONENTS Xtst)
endif()
find_package(X11 REQUIRED COMPONENTS ${GISLAND_X11_COMPONENTS})
find_package(PkgConfig REQUIRED)
pkg_check_modules(PANGOCAIRO REQUIRED IMPORTED_TARGET pangocairo)
pkg_check_modules(PANGOFT2 REQUIRED IMPORTED_TARGET pangoft2)
pkg_check_modules(CAIRO REQUIRED IMPORTED_TARGET cairo)
pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
pkg_check_modules(LUA54 REQUIRED IMPORTED_TARGET lua5.4>=5.4)
pkg_check_modules(GLIB2 REQUIRED IMPORTED_TARGET glib-2.0)
pkg_check_modules(GIO2 REQUIRED IMPORTED_TARGET gio-2.0)
if(LUA54_VERSION VERSION_LESS 5.4 OR NOT LUA54_VERSION VERSION_LESS 5.5)
  message(FATAL_ERROR "gisland requires Lua >= 5.4 and < 5.5; found ${LUA54_VERSION}")
endif()

find_program(LUA54_EXECUTABLE NAMES lua5.4 lua)
if(NOT LUA54_EXECUTABLE)
  message(FATAL_ERROR "gisland requires a Lua 5.4 interpreter (lua5.4 or lua)")
endif()
execute_process(
  COMMAND "${LUA54_EXECUTABLE}" -e "io.write(_VERSION)"
  RESULT_VARIABLE LUA54_VERSION_RESULT
  OUTPUT_VARIABLE LUA54_INTERPRETER_VERSION
  ERROR_VARIABLE LUA54_VERSION_ERROR
)
if(NOT LUA54_VERSION_RESULT EQUAL 0 OR NOT LUA54_INTERPRETER_VERSION STREQUAL "Lua 5.4")
  message(
    FATAL_ERROR
      "gisland requires a Lua 5.4 interpreter; ${LUA54_EXECUTABLE} reported "
      "'${LUA54_INTERPRETER_VERSION}' (${LUA54_VERSION_ERROR})"
  )
endif()

set(GISLAND_LGI_PREFIX "")
if(GISLAND_LGI_ROOT)
  if(EXISTS "${GISLAND_LGI_ROOT}/usr/share/lua/5.4/lgi.lua")
    set(GISLAND_LGI_PREFIX "${GISLAND_LGI_ROOT}/usr")
  elseif(EXISTS "${GISLAND_LGI_ROOT}/share/lua/5.4/lgi.lua")
    set(GISLAND_LGI_PREFIX "${GISLAND_LGI_ROOT}")
  else()
    message(
      FATAL_ERROR
        "GISLAND_LGI_ROOT does not contain share/lua/5.4/lgi.lua: ${GISLAND_LGI_ROOT}"
    )
  endif()
  set(
    GISLAND_LGI_LUA_PATH
    "${GISLAND_LGI_PREFIX}/share/lua/5.4/?.lua;${GISLAND_LGI_PREFIX}/share/lua/5.4/?/init.lua"
  )
  set(
    GISLAND_LGI_NATIVE_PATHS
    "${GISLAND_LGI_PREFIX}/lib/lua/5.4/?.so"
    "${GISLAND_LGI_PREFIX}/lib64/lua/5.4/?.so"
  )
  file(GLOB GISLAND_LGI_MULTIARCH_LIBDIRS LIST_DIRECTORIES true
       "${GISLAND_LGI_PREFIX}/lib/*")
  foreach(GISLAND_LGI_LIBDIR IN LISTS GISLAND_LGI_MULTIARCH_LIBDIRS)
    if(IS_DIRECTORY "${GISLAND_LGI_LIBDIR}/lua/5.4")
      list(APPEND GISLAND_LGI_NATIVE_PATHS "${GISLAND_LGI_LIBDIR}/lua/5.4/?.so")
    endif()
  endforeach()
  list(REMOVE_DUPLICATES GISLAND_LGI_NATIVE_PATHS)
  list(JOIN GISLAND_LGI_NATIVE_PATHS ";" GISLAND_LGI_LUA_CPATH)
  execute_process(
    COMMAND
      ${CMAKE_COMMAND} -E env "GISLAND_LGI_TEST_PREFIX=${GISLAND_LGI_PREFIX}"
      "LUA_PATH=${GISLAND_LGI_LUA_PATH}"
      "LUA_CPATH=${GISLAND_LGI_LUA_CPATH}" "${LUA54_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/tests/check_lgi.lua"
    RESULT_VARIABLE GISLAND_LGI_CHECK_RESULT
    ERROR_VARIABLE GISLAND_LGI_CHECK_ERROR
  )
else()
  execute_process(
    COMMAND "${LUA54_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/check_lgi.lua"
    RESULT_VARIABLE GISLAND_LGI_CHECK_RESULT
    ERROR_VARIABLE GISLAND_LGI_CHECK_ERROR
  )
endif()
if(NOT GISLAND_LGI_CHECK_RESULT EQUAL 0)
  message(
    FATAL_ERROR
      "gisland requires Lua 5.4-compatible lgi with GLib, Gio, GioUnix 2.0, Json-GLib 1.0, GdkPixbuf 2.0, Rsvg 2.0, and Gtk 3.0; install lgi and the required typelibs for "
      "${LUA54_EXECUTABLE} or set GISLAND_LGI_ROOT to a staged installation. "
      "Probe error: ${GISLAND_LGI_CHECK_ERROR}"
  )
endif()
message(STATUS "Found a Lua 5.4-compatible lgi with GLib, Gio, GioUnix 2.0, Json-GLib 1.0, GdkPixbuf 2.0, Rsvg 2.0, and Gtk 3.0")

file(GLOB_RECURSE GISLAND_ASSET_FILES CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/assets/*")
set(GISLAND_ASSET_STAMP "${PROJECT_BINARY_DIR}/assets/.stamp")
add_custom_command(
  OUTPUT "${GISLAND_ASSET_STAMP}"
  COMMAND ${CMAKE_COMMAND} -E remove_directory "${PROJECT_BINARY_DIR}/assets"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/assets"
          "${PROJECT_BINARY_DIR}/assets"
  COMMAND ${CMAKE_COMMAND} -E touch "${GISLAND_ASSET_STAMP}"
  DEPENDS ${GISLAND_ASSET_FILES}
  VERBATIM
)
add_custom_target(gisland_assets DEPENDS "${GISLAND_ASSET_STAMP}")

add_library(gisland_io STATIC src/line_buffer.cpp src/poll.cpp src/write_queue.cpp)
add_library(gisland::io ALIAS gisland_io)
target_compile_features(gisland_io PUBLIC cxx_std_23)
target_include_directories(
  gisland_io
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)
gisland_set_project_options(gisland_io)

add_library(gisland_glib_runtime STATIC src/glib_main_context.cpp)
add_library(gisland::glib_runtime ALIAS gisland_glib_runtime)
target_compile_features(gisland_glib_runtime PUBLIC cxx_std_23)
target_include_directories(
  gisland_glib_runtime
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)
target_link_libraries(gisland_glib_runtime PRIVATE PkgConfig::GLIB2 PkgConfig::GIO2)
gisland_set_project_options(gisland_glib_runtime)

add_library(
  gisland_lua_runtime STATIC
  src/lua_host.cpp
  src/lua_scene.cpp
  src/lua_transport.cpp
  src/lua_value.cpp
)
add_library(gisland::lua_runtime ALIAS gisland_lua_runtime)
target_compile_features(gisland_lua_runtime PUBLIC cxx_std_23)
target_include_directories(
  gisland_lua_runtime
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)
target_link_libraries(
  gisland_lua_runtime
  PUBLIC nlohmann_json::nlohmann_json PkgConfig::LUA54
  PRIVATE gisland::io
)
gisland_set_project_options(gisland_lua_runtime)

add_executable(gisland-lua-host src/lua_host_main.cpp)
target_compile_features(gisland-lua-host PRIVATE cxx_std_23)
target_link_libraries(gisland-lua-host PRIVATE gisland::glib_runtime gisland::lua_runtime)
gisland_set_project_options(gisland-lua-host)

add_library(
  gisland_app STATIC
  src/application.cpp
  src/bootstrap.cpp
  src/config.cpp
  src/control.cpp
  src/control_client.cpp
  src/control_dispatcher.cpp
  src/context.cpp
  src/file_watcher.cpp
  src/interaction.cpp
  src/ipc_server.cpp
  src/island.cpp
  src/layout.cpp
  src/module_lifecycle.cpp
  src/module_manifest.cpp
  src/module_supervisor.cpp
  src/posix_process_backend.cpp
  src/protocol.cpp
  src/raylib_renderer.cpp
  src/reload.cpp
  src/rich_text.cpp
  src/runtime.cpp
  src/scene.cpp
  src/scene_template.cpp
  src/theme.cpp
  src/x11_monitor.cpp
  src/x11_window_host.cpp
  src/x11_shape.cpp
)
add_library(gisland::app ALIAS gisland_app)
target_compile_features(gisland_app PUBLIC cxx_std_23)
target_include_directories(
  gisland_app
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)
target_link_libraries(
  gisland_app
  PUBLIC
    nlohmann_json::nlohmann_json
  PRIVATE
    gisland::io
    raylib
    tomlplusplus::tomlplusplus
    PkgConfig::PANGOCAIRO
    PkgConfig::PANGOFT2
    PkgConfig::CAIRO
    PkgConfig::FONTCONFIG
    X11::X11
    X11::Xext
    X11::Xrandr
)
gisland_set_project_options(gisland_app)
target_compile_definitions(
  gisland_app
  PRIVATE
    GISLAND_BUILD_BINDIR="${PROJECT_BINARY_DIR}"
    GISLAND_BUILD_DATA_DIR="${PROJECT_BINARY_DIR}/assets"
    GISLAND_INSTALL_DATA_DIR="${CMAKE_INSTALL_FULL_DATADIR}/gisland/distributed"
)

add_executable(gisland src/main.cpp)
target_compile_features(gisland PRIVATE cxx_std_23)
target_link_libraries(gisland PRIVATE gisland::app)
gisland_set_project_options(gisland)
add_dependencies(gisland gisland_assets)

add_executable(gislandctl src/control_main.cpp)
target_compile_features(gislandctl PRIVATE cxx_std_23)
target_link_libraries(gislandctl PRIVATE gisland::app)
gisland_set_project_options(gislandctl)

set(GISLAND_INSTALL_DATA_SUBDIR "${CMAKE_INSTALL_DATADIR}/gisland/distributed")
configure_file(
  packaging/clock-calendar.module.toml.in
  "${PROJECT_BINARY_DIR}/install/clock-calendar.module.toml"
  @ONLY
)
configure_file(
  packaging/notifications.module.toml.in
  "${PROJECT_BINARY_DIR}/install/notifications.module.toml"
  @ONLY
)
configure_file(
  packaging/battery.module.toml.in
  "${PROJECT_BINARY_DIR}/install/battery.module.toml"
  @ONLY
)
configure_file(
  packaging/audio.module.toml.in
  "${PROJECT_BINARY_DIR}/install/audio.module.toml"
  @ONLY
)
configure_file(
  packaging/lua-example.module.toml.in
  "${PROJECT_BINARY_DIR}/install/lua-example.module.toml"
  @ONLY
)
configure_file(
  packaging/gisland.service.in
  "${PROJECT_BINARY_DIR}/install/gisland.service"
  @ONLY
)
install(
  TARGETS gisland gislandctl gisland-lua-host
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
install(FILES assets/config.toml DESTINATION "${GISLAND_INSTALL_DATA_SUBDIR}")
install(DIRECTORY assets/fonts assets/themes DESTINATION "${GISLAND_INSTALL_DATA_SUBDIR}")
install(
  FILES "${PROJECT_BINARY_DIR}/install/clock-calendar.module.toml"
  RENAME module.toml
  DESTINATION "${GISLAND_INSTALL_DATA_SUBDIR}/modules/clock-calendar"
)
install(
  FILES
    assets/modules/clock-calendar/config.toml
    assets/modules/clock-calendar/view.toml
    assets/modules/clock-calendar/clock_calendar.lua
  DESTINATION "${GISLAND_INSTALL_DATA_SUBDIR}/modules/clock-calendar"
)
install(
  FILES "${PROJECT_BINARY_DIR}/install/notifications.module.toml"
  RENAME module.toml
  DESTINATION "${GISLAND_INSTALL_DATA_SUBDIR}/modules/notifications"
)
install(
  FILES assets/modules/notifications/notifications.lua
  DESTINATION "${GISLAND_INSTALL_DATA_SUBDIR}/modules/notifications"
)
install(
  FILES "${PROJECT_BINARY_DIR}/install/battery.module.toml"
  RENAME module.toml
  DESTINATION "${GISLAND_INSTALL_DATA_SUBDIR}/modules/battery"
)
install(
  FILES
    assets/modules/battery/config.toml
    assets/modules/battery/view.toml
    assets/modules/battery/battery.lua
  DESTINATION "${GISLAND_INSTALL_DATA_SUBDIR}/modules/battery"
)
install(
  FILES "${PROJECT_BINARY_DIR}/install/audio.module.toml"
  RENAME module.toml
  DESTINATION "${GISLAND_INSTALL_DATA_SUBDIR}/modules/audio"
)
install(
  FILES
    assets/modules/audio/config.toml
    assets/modules/audio/audio.lua
    assets/modules/audio/command.lua
  DESTINATION "${GISLAND_INSTALL_DATA_SUBDIR}/modules/audio"
)
install(
  FILES "${PROJECT_BINARY_DIR}/install/lua-example.module.toml"
  RENAME module.toml
  DESTINATION "${GISLAND_INSTALL_DATA_SUBDIR}/modules/lua-example"
)
install(
  FILES
    assets/modules/lua-example/config.toml
    assets/modules/lua-example/view.toml
    assets/modules/lua-example/example.lua
  DESTINATION "${GISLAND_INSTALL_DATA_SUBDIR}/modules/lua-example"
)
install(
  FILES "${PROJECT_BINARY_DIR}/install/gisland.service"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/systemd/user"
)

if(BUILD_TESTING)
  if(GISLAND_LGI_ROOT)
    add_test(
      NAME lgi_runtime_check
      COMMAND
        ${CMAKE_COMMAND} -E env "GISLAND_LGI_TEST_PREFIX=${GISLAND_LGI_PREFIX}"
        "LUA_PATH=${GISLAND_LGI_LUA_PATH}"
        "LUA_CPATH=${GISLAND_LGI_LUA_CPATH}" "${LUA54_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/check_lgi.lua"
    )
    add_test(
      NAME lgi_staged_layouts
      COMMAND
        ${CMAKE_COMMAND} -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
        -DBINARY_DIR=${PROJECT_BINARY_DIR}/lgi-layout-test
        -DSOURCE_PREFIX=${GISLAND_LGI_PREFIX}
        -DRAYLIB_SOURCE_DIR=${raylib_SOURCE_DIR}
        -DNLOHMANN_JSON_SOURCE_DIR=${nlohmann_json_SOURCE_DIR}
        -DTOMLPLUSPLUS_SOURCE_DIR=${tomlplusplus_SOURCE_DIR} -P
        ${PROJECT_SOURCE_DIR}/tests/lgi_layout_test.cmake
    )
  else()
    add_test(
      NAME lgi_runtime_check
      COMMAND "${LUA54_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/check_lgi.lua"
    )
  endif()
  if(GISLAND_LGI_ROOT)
    add_test(
      NAME notifications_contract_lua
      COMMAND
        dbus-run-session -- env "LUA_PATH=${GISLAND_LGI_LUA_PATH}"
        "LUA_CPATH=${GISLAND_LGI_LUA_CPATH}" "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/notifications/notifications_contract.py" --module-command
        "[\"$<TARGET_FILE:gisland-lua-host>\",\"${PROJECT_SOURCE_DIR}/assets/modules/notifications/notifications.lua\"]"
        --protocol-minor 8
    )
  else()
    add_test(
      NAME notifications_contract_lua
      COMMAND
        dbus-run-session -- "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/notifications/notifications_contract.py" --module-command
        "[\"$<TARGET_FILE:gisland-lua-host>\",\"${PROJECT_SOURCE_DIR}/assets/modules/notifications/notifications.lua\"]"
        --protocol-minor 8
    )
  endif()
  if(GISLAND_LGI_ROOT)
    add_test(
      NAME battery_contract_lua
      COMMAND
        dbus-run-session -- env "LUA_PATH=${GISLAND_LGI_LUA_PATH}"
        "LUA_CPATH=${GISLAND_LGI_LUA_CPATH}" "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/battery/battery_contract.py" --module-command
        "[\"$<TARGET_FILE:gisland-lua-host>\",\"${PROJECT_SOURCE_DIR}/assets/modules/battery/battery.lua\"]"
        --protocol-minor 8
    )
  else()
    add_test(
      NAME battery_contract_lua
      COMMAND
        dbus-run-session -- "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/battery/battery_contract.py" --module-command
        "[\"$<TARGET_FILE:gisland-lua-host>\",\"${PROJECT_SOURCE_DIR}/assets/modules/battery/battery.lua\"]"
        --protocol-minor 8
    )
  endif()
  add_test(
    NAME audio_command
    COMMAND
      ${CMAKE_COMMAND} -E env "GISLAND_LUA_EXECUTABLE=${LUA54_EXECUTABLE}"
      "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/audio/test_lua_command.py" -v
  )
  add_test(
    NAME audio_contract
    COMMAND
      "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/audio/audio_contract.py"
      --module-command
      "[\"$<TARGET_FILE:gisland-lua-host>\",\"${PROJECT_SOURCE_DIR}/assets/modules/audio/audio.lua\"]"
      --fake-path "${PROJECT_SOURCE_DIR}/tests/audio/fixtures"
      --init
      "{\"type\":\"init\",\"protocol\":{\"minimum\":{\"major\":1,\"minor\":8},\"maximum\":{\"major\":1,\"minor\":8}},\"instance_id\":\"audio-contract\",\"capabilities\":[\"independent-views\",\"compact-view-styles\",\"icon-roles\",\"progress-transitions\"],\"configuration\":{},\"locale\":\"C\",\"timezone\":\"UTC\"}"
  )
  if(GISLAND_LGI_ROOT)
    add_test(
      NAME clock_calendar_contract
      COMMAND
        ${CMAKE_COMMAND} -E env "LUA_PATH=${GISLAND_LGI_LUA_PATH}"
        "LUA_CPATH=${GISLAND_LGI_LUA_CPATH}" "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/clock_calendar/clock_calendar_contract.py"
        --module-command
        "[\"$<TARGET_FILE:gisland-lua-host>\",\"${PROJECT_SOURCE_DIR}/assets/modules/clock-calendar/clock_calendar.lua\"]"
    )
    add_test(
      NAME clock_calendar_lua_unit
      COMMAND
        ${CMAKE_COMMAND} -E env "LUA_PATH=${GISLAND_LGI_LUA_PATH}"
        "LUA_CPATH=${GISLAND_LGI_LUA_CPATH}" "GISLAND_CLOCK_CALENDAR_HELPER_TEST=1"
        "${LUA54_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/clock_calendar/clock_calendar_helpers_test.lua"
        "${PROJECT_SOURCE_DIR}/assets/modules/clock-calendar/clock_calendar.lua"
    )
  else()
    add_test(
      NAME clock_calendar_contract
      COMMAND
        "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/clock_calendar/clock_calendar_contract.py"
        --module-command
        "[\"$<TARGET_FILE:gisland-lua-host>\",\"${PROJECT_SOURCE_DIR}/assets/modules/clock-calendar/clock_calendar.lua\"]"
    )
    add_test(
      NAME clock_calendar_lua_unit
      COMMAND
        ${CMAKE_COMMAND} -E env "GISLAND_CLOCK_CALENDAR_HELPER_TEST=1"
        "${LUA54_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/clock_calendar/clock_calendar_helpers_test.lua"
        "${PROJECT_SOURCE_DIR}/assets/modules/clock-calendar/clock_calendar.lua"
    )
  endif()
  add_test(
    NAME install_local_script
    COMMAND bash ${PROJECT_SOURCE_DIR}/tests/install_local_script_test.sh ${PROJECT_SOURCE_DIR}
  )
  add_test(
    NAME install_layout
    COMMAND
      ${CMAKE_COMMAND}
      -DBUILD_DIR=${PROJECT_BINARY_DIR}
      -DSTAGING_DIR=${PROJECT_BINARY_DIR}/install-test
      -DINSTALL_PREFIX=${CMAKE_INSTALL_PREFIX}
      -DBINDIR=${CMAKE_INSTALL_BINDIR}
      -DDATADIR=${CMAKE_INSTALL_DATADIR}
      -P ${PROJECT_SOURCE_DIR}/tests/install_layout_test.cmake
  )
  add_test(
    NAME gislandctl_help
    COMMAND bash ${PROJECT_SOURCE_DIR}/tests/gislandctl_help_test.sh $<TARGET_FILE:gislandctl>
  )
  add_executable(fake_module tests/fake_module.cpp)
  target_compile_features(fake_module PRIVATE cxx_std_23)
  target_link_libraries(fake_module PRIVATE nlohmann_json::nlohmann_json)
  gisland_set_project_options(fake_module)

  add_executable(
    gisland_tests
    tests/application_config_test.cpp
    tests/bootstrap_test.cpp
    tests/config_test.cpp
    tests/control_client_test.cpp
    tests/control_dispatcher_test.cpp
    tests/control_test.cpp
  tests/context_test.cpp
  tests/file_watcher_test.cpp
  tests/interaction_test.cpp
    tests/ipc_server_test.cpp
    tests/island_test.cpp
    tests/layout_test.cpp
    tests/line_buffer_test.cpp
    tests/module_lifecycle_test.cpp
    tests/module_manifest_test.cpp
    tests/module_supervisor_test.cpp
    tests/posix_process_backend_test.cpp
    tests/protocol_serialization_test.cpp
    tests/protocol_test.cpp
    tests/reload_test.cpp
    tests/rich_text_test.cpp
    tests/runtime_test.cpp
    tests/scene_test.cpp
    tests/scene_template_test.cpp
    tests/theme_test.cpp
    tests/write_queue_test.cpp
    tests/x11_monitor_test.cpp
  )
  target_compile_features(gisland_tests PRIVATE cxx_std_23)
  target_link_libraries(
    gisland_tests
    PRIVATE
      gisland::app
      Catch2::Catch2WithMain
      X11::X11
      X11::Xext
  )
  target_compile_definitions(
    gisland_tests
    PRIVATE
      GISLAND_FAKE_MODULE_PATH="$<TARGET_FILE:fake_module>"
      GISLAND_LUA_HOST_PATH="$<TARGET_FILE:gisland-lua-host>"
      GISLAND_TEST_ASSET_ROOT="${PROJECT_BINARY_DIR}/assets"
  )
  add_dependencies(gisland_tests fake_module gisland-lua-host gisland_assets)
  gisland_set_project_options(gisland_tests)
  list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
  include(Catch)
  set(GISLAND_UNIT_TEST_PROPERTIES)
  if(GISLAND_ENABLE_SANITIZERS)
    list(
      APPEND GISLAND_UNIT_TEST_PROPERTIES
      ENVIRONMENT "LSAN_OPTIONS=suppressions=${PROJECT_SOURCE_DIR}/tests/lsan.supp"
    )
  endif()
  catch_discover_tests(
    gisland_tests
    PROPERTIES ${GISLAND_UNIT_TEST_PROPERTIES}
    DISCOVERY_MODE PRE_TEST
  )

  add_executable(gisland_lua_value_tests tests/lua_value_test.cpp)
  target_compile_features(gisland_lua_value_tests PRIVATE cxx_std_23)
  target_link_libraries(
    gisland_lua_value_tests
    PRIVATE gisland::lua_runtime Catch2::Catch2WithMain
  )
  gisland_set_project_options(gisland_lua_value_tests)
  catch_discover_tests(
    gisland_lua_value_tests
    TEST_PREFIX "lua_value::"
    PROPERTIES ${GISLAND_UNIT_TEST_PROPERTIES}
    DISCOVERY_MODE PRE_TEST
  )

  add_executable(gisland_lua_host_tests tests/lua_host_test.cpp)
  target_compile_features(gisland_lua_host_tests PRIVATE cxx_std_23)
  target_link_libraries(
    gisland_lua_host_tests
    PRIVATE
      gisland::glib_runtime
      gisland::lua_runtime
      Catch2::Catch2WithMain
      PkgConfig::GLIB2
  )
  target_compile_definitions(
    gisland_lua_host_tests
    PRIVATE
      GISLAND_LUA_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures/lua"
      GISLAND_LUA_HOST_PATH="$<TARGET_FILE:gisland-lua-host>"
  )
  if(GISLAND_LGI_PREFIX)
    target_compile_definitions(
      gisland_lua_host_tests
      PRIVATE GISLAND_TEST_LGI_PREFIX="${GISLAND_LGI_PREFIX}"
    )
  endif()
  add_dependencies(gisland_lua_host_tests gisland-lua-host)
  gisland_set_project_options(gisland_lua_host_tests)
  catch_discover_tests(
    gisland_lua_host_tests
    TEST_PREFIX "lua_host_transport::"
    PROPERTIES ${GISLAND_UNIT_TEST_PROPERTIES}
    DISCOVERY_MODE PRE_TEST
  )

  add_executable(gisland_lua_scene_tests tests/lua_scene_test.cpp)
  target_compile_features(gisland_lua_scene_tests PRIVATE cxx_std_23)
  target_link_libraries(
    gisland_lua_scene_tests
    PRIVATE gisland::lua_runtime Catch2::Catch2WithMain
  )
  gisland_set_project_options(gisland_lua_scene_tests)
  catch_discover_tests(
    gisland_lua_scene_tests
    TEST_PREFIX "lua_scene::"
    PROPERTIES ${GISLAND_UNIT_TEST_PROPERTIES}
    DISCOVERY_MODE PRE_TEST
  )
  if(GISLAND_BUILD_GRAPHICAL_TESTS)
    add_executable(gisland_application_x11_smoke_tests tests/application_x11_smoke_test.cpp)
    target_compile_features(gisland_application_x11_smoke_tests PRIVATE cxx_std_23)
    target_link_libraries(
      gisland_application_x11_smoke_tests
      PRIVATE gisland::app Catch2::Catch2WithMain X11::X11 X11::Xtst
    )
    target_compile_definitions(
      gisland_application_x11_smoke_tests
      PRIVATE
        GISLAND_BINARY_PATH="$<TARGET_FILE:gisland>"
        GISLANDCTL_BINARY_PATH="$<TARGET_FILE:gislandctl>"
        GISLAND_FAKE_MODULE_PATH="$<TARGET_FILE:fake_module>"
        GISLAND_LUA_HOST_PATH="$<TARGET_FILE:gisland-lua-host>"
        GISLAND_AUDIO_LUA_PATH="${PROJECT_SOURCE_DIR}/assets/modules/audio/audio.lua"
        GISLAND_AUDIO_FAKE_PACTL_PATH="${PROJECT_SOURCE_DIR}/tests/audio/fixtures/fake-pactl"
        GISLAND_AUDIO_FAKE_GISLANDCTL_PATH="${PROJECT_SOURCE_DIR}/tests/audio/fixtures/fake-gislandctl"
        GISLAND_TEST_ASSET_ROOT="${PROJECT_BINARY_DIR}/assets"
    )
    if(GISLAND_LGI_PREFIX)
      target_compile_definitions(
        gisland_application_x11_smoke_tests
        PRIVATE GISLAND_TEST_LGI_PREFIX="${GISLAND_LGI_PREFIX}"
      )
    endif()
    gisland_set_project_options(gisland_application_x11_smoke_tests)
    add_dependencies(
      gisland_application_x11_smoke_tests
      gisland
      gislandctl
      gisland-lua-host
      fake_module
    )
    catch_discover_tests(
      gisland_application_x11_smoke_tests
      TEST_PREFIX "application_x11::"
      PROPERTIES LABELS graphical RUN_SERIAL true
      DISCOVERY_MODE PRE_TEST
    )

    add_executable(gisland_x11_window_tests tests/x11_window_host_test.cpp)
    target_compile_features(gisland_x11_window_tests PRIVATE cxx_std_23)
    target_link_libraries(
      gisland_x11_window_tests
      PRIVATE gisland::app Catch2::Catch2WithMain X11::X11 X11::Xext X11::Xtst
    )
    gisland_set_project_options(gisland_x11_window_tests)
    catch_discover_tests(
      gisland_x11_window_tests
      TEST_PREFIX "x11_window::"
      PROPERTIES LABELS graphical RUN_SERIAL true
      DISCOVERY_MODE PRE_TEST
    )

    add_executable(gisland_raylib_renderer_tests tests/raylib_renderer_test.cpp)
    target_compile_features(gisland_raylib_renderer_tests PRIVATE cxx_std_23)
    target_link_libraries(
      gisland_raylib_renderer_tests
      PRIVATE gisland::app Catch2::Catch2WithMain raylib
    )
    target_compile_definitions(
      gisland_raylib_renderer_tests
      PRIVATE GISLAND_TEST_ASSET_ROOT="${PROJECT_BINARY_DIR}/assets"
    )
    gisland_set_project_options(gisland_raylib_renderer_tests)
    add_dependencies(gisland_raylib_renderer_tests gisland_assets)
    catch_discover_tests(
      gisland_raylib_renderer_tests
      TEST_PREFIX "raylib_renderer::"
      PROPERTIES LABELS graphical
      DISCOVERY_MODE PRE_TEST
    )

    add_executable(gisland_visual_regression_tests tests/visual_regression_test.cpp)
    target_compile_features(gisland_visual_regression_tests PRIVATE cxx_std_23)
    target_link_libraries(
      gisland_visual_regression_tests
      PRIVATE gisland::app Catch2::Catch2WithMain raylib
    )
    target_compile_definitions(
      gisland_visual_regression_tests
      PRIVATE
        GISLAND_TEST_ASSET_ROOT="${PROJECT_BINARY_DIR}/assets"
        GISLAND_TEST_BASELINE_ROOT="${PROJECT_SOURCE_DIR}/tests/baselines"
        GISLAND_VISUAL_ARTIFACT_ROOT="${PROJECT_BINARY_DIR}/visual-artifacts"
        GISLAND_LUA_HOST_PATH="$<TARGET_FILE:gisland-lua-host>"
        GISLAND_AUDIO_LUA_PATH="${PROJECT_SOURCE_DIR}/assets/modules/audio/audio.lua"
        GISLAND_AUDIO_FAKE_PACTL_PATH="${PROJECT_SOURCE_DIR}/tests/audio/fixtures/fake-pactl"
        GISLAND_AUDIO_FAKE_GISLANDCTL_PATH="${PROJECT_SOURCE_DIR}/tests/audio/fixtures/fake-gislandctl"
    )
    gisland_set_project_options(gisland_visual_regression_tests)
    add_dependencies(gisland_visual_regression_tests gisland-lua-host gisland_assets)
    catch_discover_tests(
      gisland_visual_regression_tests
      TEST_PREFIX "visual_regression::"
      PROPERTIES LABELS graphical
      DISCOVERY_MODE PRE_TEST
    )
    add_custom_target(
      update-visual-baselines
      COMMAND
        ${CMAKE_COMMAND} -E env GISLAND_UPDATE_BASELINES=1 GISLAND_BASELINE_UPDATE_TARGET=1
        $<TARGET_FILE:gisland_visual_regression_tests>
      DEPENDS gisland_visual_regression_tests
      VERBATIM
    )
  endif()
endif()

set(
  GISLAND_FORMAT_SOURCES
  include/gisland/application.hpp
  include/gisland/bootstrap.hpp
  include/gisland/content_fingerprint.hpp
  include/gisland/config.hpp
  include/gisland/control.hpp
  include/gisland/control_client.hpp
  include/gisland/control_dispatcher.hpp
  include/gisland/context.hpp
  include/gisland/file_watcher.hpp
  include/gisland/glib_main_context.hpp
  include/gisland/interaction.hpp
  include/gisland/ipc_server.hpp
  include/gisland/island.hpp
  include/gisland/layout.hpp
  include/gisland/line_buffer.hpp
  include/gisland/lua_value.hpp
  include/gisland/lua_scene.hpp
  include/gisland/lua_transport.hpp
  include/gisland/lua_host.hpp
  include/gisland/module_lifecycle.hpp
  include/gisland/module_manifest.hpp
  include/gisland/module_supervisor.hpp
  include/gisland/poll.hpp
  include/gisland/process_backend.hpp
  include/gisland/protocol.hpp
  include/gisland/raylib_renderer.hpp
  include/gisland/reload.hpp
  include/gisland/rich_text.hpp
  include/gisland/runtime.hpp
  include/gisland/scene.hpp
  include/gisland/scene_template.hpp
  include/gisland/theme.hpp
  include/gisland/write_queue.hpp
  include/gisland/x11_monitor.hpp
  include/gisland/x11_window_host.hpp
  include/gisland/x11_shape.hpp
  src/application.cpp
  src/bootstrap.cpp
  src/config.cpp
  src/control.cpp
  src/control_client.cpp
  src/control_main.cpp
  src/control_dispatcher.cpp
  src/context.cpp
  src/file_watcher.cpp
  src/glib_main_context.cpp
  src/interaction.cpp
  src/ipc_server.cpp
  src/island.cpp
  src/layout.cpp
  src/line_buffer.cpp
  src/lua_value.cpp
  src/lua_scene.cpp
  src/lua_transport.cpp
  src/lua_host.cpp
  src/lua_host_main.cpp
  src/main.cpp
  src/module_lifecycle.cpp
  src/module_manifest.cpp
  src/module_supervisor.cpp
  src/poll.cpp
  src/posix_process_backend.cpp
  src/protocol.cpp
  src/raylib_renderer.cpp
  src/reload.cpp
  src/rich_text.cpp
  src/runtime.cpp
  src/scene.cpp
  src/scene_template.cpp
  src/theme.cpp
  src/write_queue.cpp
  src/x11_monitor.cpp
  src/x11_window_host.cpp
  src/x11_shape.cpp
  tests/fake_module.cpp
  tests/audio/audio_process.hpp
  tests/application_x11_smoke_test.cpp
  tests/application_config_test.cpp
  tests/bootstrap_test.cpp
  tests/config_test.cpp
  tests/control_client_test.cpp
  tests/control_dispatcher_test.cpp
  tests/control_test.cpp
  tests/context_test.cpp
  tests/file_watcher_test.cpp
  tests/interaction_test.cpp
  tests/ipc_server_test.cpp
  tests/island_test.cpp
  tests/layout_test.cpp
  tests/line_buffer_test.cpp
  tests/lua_value_test.cpp
  tests/lua_host_test.cpp
  tests/lua_scene_test.cpp
  tests/module_lifecycle_test.cpp
  tests/module_manifest_test.cpp
  tests/module_supervisor_test.cpp
  tests/posix_process_backend_test.cpp
  tests/protocol_serialization_test.cpp
  tests/protocol_test.cpp
  tests/reload_test.cpp
  tests/rich_text_test.cpp
  tests/runtime_test.cpp
  tests/raylib_renderer_test.cpp
  tests/scene_test.cpp
  tests/scene_template_test.cpp
  tests/theme_test.cpp
  tests/visual_regression_test.cpp
  tests/write_queue_test.cpp
  tests/x11_monitor_test.cpp
  tests/x11_window_host_test.cpp
)
find_program(CLANG_FORMAT_EXECUTABLE NAMES clang-format)
if(CLANG_FORMAT_EXECUTABLE)
  add_custom_target(
    format
    COMMAND ${CLANG_FORMAT_EXECUTABLE} -i ${GISLAND_FORMAT_SOURCES}
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    VERBATIM
  )
  add_custom_target(
    format-check
    COMMAND ${CLANG_FORMAT_EXECUTABLE} --dry-run --Werror ${GISLAND_FORMAT_SOURCES}
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    VERBATIM
  )
endif()
