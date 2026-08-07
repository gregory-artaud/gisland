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

find_package(Python3 REQUIRED COMPONENTS Interpreter)

option(GISLAND_BUILD_GRAPHICAL_TESTS "Build tests that require an X11/OpenGL display" OFF)

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

add_library(gisland_clock_calendar_model STATIC src/clock_calendar.cpp)
add_library(gisland::clock_calendar_model ALIAS gisland_clock_calendar_model)
target_compile_features(gisland_clock_calendar_model PUBLIC cxx_std_23)
target_include_directories(
  gisland_clock_calendar_model
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)
gisland_set_project_options(gisland_clock_calendar_model)

add_executable(gisland_clock_calendar src/clock_calendar_main.cpp)
set_target_properties(gisland_clock_calendar PROPERTIES OUTPUT_NAME gisland-clock-calendar)
target_compile_features(gisland_clock_calendar PRIVATE cxx_std_23)
target_link_libraries(
  gisland_clock_calendar
  PRIVATE gisland::clock_calendar_model nlohmann_json::nlohmann_json
)
gisland_set_project_options(gisland_clock_calendar)

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
  src/line_buffer.cpp
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
  src/write_queue.cpp
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
set(GISLAND_NOTIFICATION_PYTHON_ROOT "${PROJECT_SOURCE_DIR}/modules/notifications")
configure_file(
  packaging/gisland-notifications.in
  "${PROJECT_BINARY_DIR}/gisland-notifications"
  @ONLY
)
configure_file(
  packaging/gisland-notification-history.in
  "${PROJECT_BINARY_DIR}/gisland-notification-history"
  @ONLY
)
file(
  CHMOD "${PROJECT_BINARY_DIR}/gisland-notifications"
  PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
)
file(
  CHMOD "${PROJECT_BINARY_DIR}/gisland-notification-history"
  PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
)
set(
  GISLAND_NOTIFICATION_PYTHON_ROOT
  "${CMAKE_INSTALL_FULL_DATADIR}/gisland/notifications"
)
configure_file(
  packaging/gisland-notifications.in
  "${PROJECT_BINARY_DIR}/install/gisland-notifications"
  @ONLY
)
configure_file(
  packaging/gisland-notification-history.in
  "${PROJECT_BINARY_DIR}/install/gisland-notification-history"
  @ONLY
)
configure_file(
  packaging/gisland.service.in
  "${PROJECT_BINARY_DIR}/install/gisland.service"
  @ONLY
)
install(
  TARGETS gisland gislandctl gisland_clock_calendar
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
  FILES "${PROJECT_BINARY_DIR}/install/notifications.module.toml"
  RENAME module.toml
  DESTINATION "${GISLAND_INSTALL_DATA_SUBDIR}/modules/notifications"
)
install(
  PROGRAMS
    "${PROJECT_BINARY_DIR}/install/gisland-notifications"
    "${PROJECT_BINARY_DIR}/install/gisland-notification-history"
  DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
install(
  DIRECTORY modules/notifications/gisland_notifications
  DESTINATION "${CMAKE_INSTALL_DATADIR}/gisland/notifications"
  PATTERN "__pycache__" EXCLUDE
)
install(
  FILES "${PROJECT_BINARY_DIR}/install/gisland.service"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/systemd/user"
)

if(BUILD_TESTING)
  add_test(
    NAME notifications_python
    COMMAND
      dbus-run-session -- env "PYTHONPATH=${PROJECT_SOURCE_DIR}/modules/notifications"
      "${Python3_EXECUTABLE}" -m unittest discover -s
      "${PROJECT_SOURCE_DIR}/tests/notifications" -p "test_*.py" -v
  )
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
  add_executable(fake_module tests/fake_module.cpp)
  target_compile_features(fake_module PRIVATE cxx_std_23)
  target_link_libraries(fake_module PRIVATE nlohmann_json::nlohmann_json)
  gisland_set_project_options(fake_module)

  add_executable(
    gisland_tests
    tests/application_config_test.cpp
    tests/bootstrap_test.cpp
    tests/clock_calendar_test.cpp
    tests/clock_calendar_process_test.cpp
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
      gisland::clock_calendar_model
      Catch2::Catch2WithMain
      X11::X11
      X11::Xext
  )
  target_compile_definitions(
    gisland_tests
    PRIVATE
      GISLAND_FAKE_MODULE_PATH="$<TARGET_FILE:fake_module>"
      GISLAND_CLOCK_CALENDAR_PATH="${PROJECT_BINARY_DIR}/gisland-clock-calendar"
      GISLAND_TEST_ASSET_ROOT="${PROJECT_BINARY_DIR}/assets"
  )
  add_dependencies(gisland_tests fake_module gisland_clock_calendar gisland_assets)
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
        GISLAND_CLOCK_CALENDAR_PATH="$<TARGET_FILE:gisland_clock_calendar>"
        GISLAND_FAKE_MODULE_PATH="$<TARGET_FILE:fake_module>"
        GISLAND_NOTIFICATION_HISTORY_PATH="${PROJECT_BINARY_DIR}/gisland-notification-history"
    )
    gisland_set_project_options(gisland_application_x11_smoke_tests)
    add_dependencies(
      gisland_application_x11_smoke_tests
      gisland
      gisland_clock_calendar
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
    )
    gisland_set_project_options(gisland_visual_regression_tests)
    add_dependencies(gisland_visual_regression_tests gisland_assets)
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
  include/gisland/clock_calendar.hpp
  include/gisland/config.hpp
  include/gisland/control.hpp
  include/gisland/control_client.hpp
  include/gisland/control_dispatcher.hpp
  include/gisland/context.hpp
  include/gisland/file_watcher.hpp
  include/gisland/interaction.hpp
  include/gisland/ipc_server.hpp
  include/gisland/island.hpp
  include/gisland/layout.hpp
  include/gisland/line_buffer.hpp
  include/gisland/module_lifecycle.hpp
  include/gisland/module_manifest.hpp
  include/gisland/module_supervisor.hpp
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
  src/clock_calendar.cpp
  src/clock_calendar_main.cpp
  src/config.cpp
  src/control.cpp
  src/control_client.cpp
  src/control_main.cpp
  src/control_dispatcher.cpp
  src/context.cpp
  src/file_watcher.cpp
  src/interaction.cpp
  src/ipc_server.cpp
  src/island.cpp
  src/layout.cpp
  src/line_buffer.cpp
  src/main.cpp
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
  src/write_queue.cpp
  src/x11_monitor.cpp
  src/x11_window_host.cpp
  src/x11_shape.cpp
  tests/fake_module.cpp
  tests/application_x11_smoke_test.cpp
  tests/application_config_test.cpp
  tests/bootstrap_test.cpp
  tests/clock_calendar_test.cpp
  tests/clock_calendar_process_test.cpp
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
