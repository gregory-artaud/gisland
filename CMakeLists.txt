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
option(GISLAND_BUILD_WAYLAND_PROTOTYPE "Build the isolated Wayland/EGL/rlgl prototype" OFF)

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

if(GISLAND_BUILD_WAYLAND_PROTOTYPE)
  enable_language(C)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
  pkg_check_modules(WAYLAND_EGL REQUIRED IMPORTED_TARGET wayland-egl)
  pkg_check_modules(EGL REQUIRED IMPORTED_TARGET egl)
  find_program(WAYLAND_SCANNER_EXECUTABLE NAMES wayland-scanner REQUIRED)
  FetchContent_Declare(
    wlr_protocols
    GIT_REPOSITORY https://gitlab.freedesktop.org/wlroots/wlr-protocols.git
    GIT_TAG bf4fc79abc359eea5a0edec0ac6d4a2b2955f82a
    GIT_SHALLOW FALSE
  )
  FetchContent_MakeAvailable(wlr_protocols)

  set(
    GISLAND_LAYER_SHELL_XML
    "${wlr_protocols_SOURCE_DIR}/unstable/wlr-layer-shell-unstable-v1.xml"
  )
  set(GISLAND_WAYLAND_GENERATED_DIR "${PROJECT_BINARY_DIR}/generated/wayland")
  set(
    GISLAND_LAYER_SHELL_HEADER
    "${GISLAND_WAYLAND_GENERATED_DIR}/wlr-layer-shell-unstable-v1-client-protocol.h"
  )
  set(
    GISLAND_LAYER_SHELL_RAW_HEADER
    "${GISLAND_WAYLAND_GENERATED_DIR}/wlr-layer-shell-unstable-v1-client-protocol.raw.h"
  )
  set(
    GISLAND_LAYER_SHELL_CODE
    "${GISLAND_WAYLAND_GENERATED_DIR}/wlr-layer-shell-unstable-v1-protocol.c"
  )
  add_custom_command(
    OUTPUT
      "${GISLAND_LAYER_SHELL_HEADER}"
      "${GISLAND_LAYER_SHELL_CODE}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GISLAND_WAYLAND_GENERATED_DIR}"
    COMMAND
      "${WAYLAND_SCANNER_EXECUTABLE}" client-header "${GISLAND_LAYER_SHELL_XML}"
      "${GISLAND_LAYER_SHELL_RAW_HEADER}"
    COMMAND
      ${CMAKE_COMMAND} -DINPUT=${GISLAND_LAYER_SHELL_RAW_HEADER}
      -DOUTPUT=${GISLAND_LAYER_SHELL_HEADER}
      -P "${PROJECT_SOURCE_DIR}/cmake/SanitizeLayerShellHeader.cmake"
    COMMAND
      "${WAYLAND_SCANNER_EXECUTABLE}" private-code "${GISLAND_LAYER_SHELL_XML}"
      "${GISLAND_LAYER_SHELL_CODE}"
    DEPENDS
      "${GISLAND_LAYER_SHELL_XML}"
      "${PROJECT_SOURCE_DIR}/cmake/SanitizeLayerShellHeader.cmake"
    BYPRODUCTS "${GISLAND_LAYER_SHELL_RAW_HEADER}"
    VERBATIM
  )

  add_library(gisland_wayland_prototype_options STATIC prototype/wayland/options.cpp)
  target_compile_features(gisland_wayland_prototype_options PUBLIC cxx_std_23)
  target_include_directories(
    gisland_wayland_prototype_options
    PUBLIC ${PROJECT_SOURCE_DIR}/prototype/wayland
  )
  gisland_set_project_options(gisland_wayland_prototype_options)

  add_library(gisland_wayland_protocol STATIC "${GISLAND_LAYER_SHELL_CODE}")
  set_source_files_properties(
    "${GISLAND_LAYER_SHELL_CODE}"
    PROPERTIES LANGUAGE C
  )

  add_executable(
    gisland_wayland_prototype
    prototype/wayland/main.cpp
    prototype/wayland/runtime.cpp
    prototype/wayland/xdg_popup_interface.cpp
    tests/fixtures/primitive_gallery.cpp
    "${GISLAND_LAYER_SHELL_HEADER}"
  )
  target_compile_features(gisland_wayland_prototype PRIVATE cxx_std_23)
  target_include_directories(
    gisland_wayland_prototype
    PRIVATE
      ${PROJECT_SOURCE_DIR}/prototype/wayland
      ${PROJECT_SOURCE_DIR}/tests/fixtures
      ${GISLAND_WAYLAND_GENERATED_DIR}
  )
  target_compile_definitions(gisland_wayland_prototype PRIVATE GRAPHICS_API_OPENGL_33)
  target_compile_definitions(
    gisland_wayland_prototype
    PRIVATE
      GISLAND_TEST_ASSET_ROOT="${PROJECT_BINARY_DIR}/assets"
  )
  target_link_libraries(
    gisland_wayland_prototype
    PRIVATE
      gisland_wayland_prototype_options
      PkgConfig::WAYLAND_CLIENT
      PkgConfig::WAYLAND_EGL
      PkgConfig::EGL
      gisland_rlgl_gpu
      gisland_scene_layout
      gisland_wayland_protocol
  )
  gisland_set_project_options(gisland_wayland_prototype)

  if(BUILD_TESTING)
    add_executable(
      gisland_rlgl_x11_gallery_reference
      tests/fixtures/primitive_gallery.cpp
      tests/rlgl_x11_gallery_reference.cpp
    )
    target_compile_features(gisland_rlgl_x11_gallery_reference PRIVATE cxx_std_23)
    target_include_directories(
      gisland_rlgl_x11_gallery_reference
      PRIVATE ${PROJECT_SOURCE_DIR}/tests/fixtures
    )
    target_compile_definitions(
      gisland_rlgl_x11_gallery_reference
      PRIVATE GISLAND_TEST_ASSET_ROOT="${PROJECT_BINARY_DIR}/assets"
    )
    target_link_libraries(
      gisland_rlgl_x11_gallery_reference
      PRIVATE gisland_rlgl_gpu gisland_scene_layout PkgConfig::EGL
    )
    gisland_set_project_options(gisland_rlgl_x11_gallery_reference)

    add_executable(
      gisland_raylib_gallery_reference
      tests/fixtures/primitive_gallery.cpp
      tests/raylib_gallery_reference.cpp
    )
    target_compile_features(gisland_raylib_gallery_reference PRIVATE cxx_std_23)
    target_include_directories(
      gisland_raylib_gallery_reference
      PRIVATE ${PROJECT_SOURCE_DIR}/tests/fixtures
    )
    target_compile_definitions(
      gisland_raylib_gallery_reference
      PRIVATE GISLAND_TEST_ASSET_ROOT="${PROJECT_BINARY_DIR}/assets"
    )
    target_link_libraries(gisland_raylib_gallery_reference PRIVATE gisland_app raylib)
    gisland_set_project_options(gisland_raylib_gallery_reference)

    add_executable(
      gisland_wayland_prototype_tests
      tests/wayland_prototype_options_test.cpp
    )
    target_compile_features(gisland_wayland_prototype_tests PRIVATE cxx_std_23)
    target_link_libraries(
      gisland_wayland_prototype_tests
      PRIVATE gisland_wayland_prototype_options Catch2::Catch2WithMain
    )
    gisland_set_project_options(gisland_wayland_prototype_tests)
    add_test(NAME wayland_prototype_options COMMAND gisland_wayland_prototype_tests)
    find_program(GISLAND_SWAY_EXECUTABLE NAMES sway)
    if(GISLAND_SWAY_EXECUTABLE)
      find_program(GISLAND_XVFB_EXECUTABLE NAMES Xvfb REQUIRED)
      add_test(
        NAME wayland_rlgl_prototype
        COMMAND
          bash ${PROJECT_SOURCE_DIR}/scripts/run-wayland-prototype-test.sh
          "${GISLAND_SWAY_EXECUTABLE}"
          "${GISLAND_XVFB_EXECUTABLE}"
          $<TARGET_FILE:gisland_raylib_gallery_reference>
          $<TARGET_FILE:gisland_rlgl_x11_gallery_reference>
          $<TARGET_FILE:gisland_wayland_prototype>
      )
      set_tests_properties(
        wayland_rlgl_prototype
        PROPERTIES LABELS graphical RUN_SERIAL true TIMEOUT 30
      )
    else()
      message(
        STATUS
        "Wayland prototype runtime test disabled: install sway for a headless layer-shell compositor"
      )
    endif()
  endif()
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
if(LUA54_VERSION VERSION_LESS 5.4 OR NOT LUA54_VERSION VERSION_LESS 5.5)
  message(FATAL_ERROR "gisland requires Lua >= 5.4 and < 5.5; found ${LUA54_VERSION}")
endif()

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
if(TARGET gisland_wayland_prototype)
  add_dependencies(gisland_wayland_prototype gisland_assets)
endif()
if(TARGET gisland_rlgl_x11_gallery_reference)
  add_dependencies(gisland_rlgl_x11_gallery_reference gisland_assets)
endif()
if(TARGET gisland_raylib_gallery_reference)
  add_dependencies(gisland_raylib_gallery_reference gisland_assets)
endif()

add_library(gisland_theme STATIC src/theme.cpp)
add_library(gisland::theme ALIAS gisland_theme)
target_compile_features(gisland_theme PUBLIC cxx_std_23)
target_include_directories(
  gisland_theme
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)
target_link_libraries(gisland_theme PRIVATE tomlplusplus::tomlplusplus)
gisland_set_project_options(gisland_theme)

add_library(gisland_rich_text STATIC src/rich_text.cpp)
add_library(gisland::rich_text ALIAS gisland_rich_text)
target_compile_features(gisland_rich_text PUBLIC cxx_std_23)
target_include_directories(
  gisland_rich_text
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)
target_link_libraries(
  gisland_rich_text
  PRIVATE
    PkgConfig::PANGOCAIRO
    PkgConfig::PANGOFT2
    PkgConfig::CAIRO
    PkgConfig::FONTCONFIG
)
gisland_set_project_options(gisland_rich_text)

add_library(gisland_scene_layout STATIC src/layout.cpp src/scene.cpp)
add_library(gisland::scene_layout ALIAS gisland_scene_layout)
target_compile_features(gisland_scene_layout PUBLIC cxx_std_23)
target_include_directories(
  gisland_scene_layout
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)
target_link_libraries(gisland_scene_layout PUBLIC gisland::rich_text gisland::theme)
gisland_set_project_options(gisland_scene_layout)

add_library(gisland_clock_calendar_model STATIC src/clock_calendar.cpp)
add_library(gisland::clock_calendar_model ALIAS gisland_clock_calendar_model)
target_compile_features(gisland_clock_calendar_model PUBLIC cxx_std_23)
target_include_directories(
  gisland_clock_calendar_model
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)
gisland_set_project_options(gisland_clock_calendar_model)

add_library(
  gisland_rlgl_renderer_model STATIC
  src/rlgl_font_book.cpp
  src/rlgl_renderer_model.cpp
)
add_library(gisland::rlgl_renderer_model ALIAS gisland_rlgl_renderer_model)
set_source_files_properties(src/rlgl_font_book.cpp PROPERTIES COMPILE_OPTIONS "-w")
target_compile_features(gisland_rlgl_renderer_model PUBLIC cxx_std_23)
target_include_directories(
  gisland_rlgl_renderer_model
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  PRIVATE ${raylib_SOURCE_DIR}/src/external
)
gisland_set_project_options(gisland_rlgl_renderer_model)

add_library(
  gisland_rlgl_gpu STATIC
  src/rlgl_gpu.cpp
  src/rlgl_impl.cpp
  src/rlgl_painter.cpp
  src/rlgl_texture_books.cpp
)
add_library(gisland::rlgl_gpu ALIAS gisland_rlgl_gpu)
target_compile_features(gisland_rlgl_gpu PUBLIC cxx_std_23)
target_compile_definitions(gisland_rlgl_gpu PRIVATE GRAPHICS_API_OPENGL_33)
target_include_directories(
  gisland_rlgl_gpu
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  PRIVATE ${raylib_SOURCE_DIR}/src
)
target_link_libraries(
  gisland_rlgl_gpu
  PUBLIC gisland::rlgl_renderer_model
  PRIVATE gisland::rich_text
)
set_source_files_properties(src/rlgl_impl.cpp PROPERTIES COMPILE_OPTIONS "-w")
gisland_set_project_options(gisland_rlgl_gpu)

add_library(gisland_io STATIC src/line_buffer.cpp src/write_queue.cpp)
add_library(gisland::io ALIAS gisland_io)
target_compile_features(gisland_io PUBLIC cxx_std_23)
target_include_directories(
  gisland_io
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)
gisland_set_project_options(gisland_io)

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

add_executable(gisland_clock_calendar src/clock_calendar_main.cpp)
set_target_properties(gisland_clock_calendar PROPERTIES OUTPUT_NAME gisland-clock-calendar)
target_compile_features(gisland_clock_calendar PRIVATE cxx_std_23)
target_link_libraries(
  gisland_clock_calendar
  PRIVATE gisland::clock_calendar_model nlohmann_json::nlohmann_json
)
gisland_set_project_options(gisland_clock_calendar)

add_executable(gisland-lua-host src/lua_host_main.cpp)
target_compile_features(gisland-lua-host PRIVATE cxx_std_23)
target_link_libraries(gisland-lua-host PRIVATE gisland::lua_runtime)
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
  src/display.cpp
  src/file_watcher.cpp
  src/interaction.cpp
  src/ipc_server.cpp
  src/island.cpp
  src/module_lifecycle.cpp
  src/module_manifest.cpp
  src/module_supervisor.cpp
  src/posix_process_backend.cpp
  src/protocol.cpp
  src/raylib_renderer.cpp
  src/reload.cpp
  src/runtime.cpp
  src/scene_template.cpp
  src/x11_platform_host.cpp
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
    gisland::rich_text
    gisland::scene_layout
    gisland::theme
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
set(GISLAND_BATTERY_PYTHON_ROOT "${PROJECT_SOURCE_DIR}/modules/battery")
configure_file(
  packaging/gisland-battery.in
  "${PROJECT_BINARY_DIR}/gisland-battery"
  @ONLY
)
file(
  CHMOD "${PROJECT_BINARY_DIR}/gisland-battery"
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
set(GISLAND_BATTERY_PYTHON_ROOT "${CMAKE_INSTALL_FULL_DATADIR}/gisland/battery")
configure_file(
  packaging/gisland-battery.in
  "${PROJECT_BINARY_DIR}/install/gisland-battery"
  @ONLY
)
configure_file(
  packaging/gisland.service.in
  "${PROJECT_BINARY_DIR}/install/gisland.service"
  @ONLY
)
install(
  TARGETS gisland gislandctl gisland-lua-host gisland_clock_calendar
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
  FILES "${PROJECT_BINARY_DIR}/install/battery.module.toml"
  RENAME module.toml
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
  PROGRAMS
    "${PROJECT_BINARY_DIR}/install/gisland-notifications"
    "${PROJECT_BINARY_DIR}/install/gisland-notification-history"
    "${PROJECT_BINARY_DIR}/install/gisland-battery"
  DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
install(
  DIRECTORY modules/notifications/gisland_notifications
  DESTINATION "${CMAKE_INSTALL_DATADIR}/gisland/notifications"
  PATTERN "__pycache__" EXCLUDE
)
install(
  DIRECTORY modules/battery/gisland_battery
  DESTINATION "${CMAKE_INSTALL_DATADIR}/gisland/battery"
  PATTERN "__pycache__" EXCLUDE
)
install(
  FILES "${PROJECT_BINARY_DIR}/install/gisland.service"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/systemd/user"
)

if(BUILD_TESTING)
  find_program(LUA54_EXECUTABLE NAMES lua5.4 lua)
  if(NOT LUA54_EXECUTABLE)
    message(FATAL_ERROR "gisland tests require a Lua 5.4 interpreter (lua5.4 or lua)")
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
        "gisland tests require a Lua 5.4 interpreter; ${LUA54_EXECUTABLE} reported "
        "'${LUA54_INTERPRETER_VERSION}' (${LUA54_VERSION_ERROR})"
    )
  endif()
  add_test(
    NAME notifications_python
    COMMAND
      dbus-run-session -- env "PYTHONPATH=${PROJECT_SOURCE_DIR}/modules/notifications"
      "${Python3_EXECUTABLE}" -m unittest discover -s
      "${PROJECT_SOURCE_DIR}/tests/notifications" -p "test_*.py" -v
  )
  add_test(
    NAME battery_python
    COMMAND
      env "PYTHONPATH=${PROJECT_SOURCE_DIR}/modules/battery"
      "GISLAND_BATTERY_PATH=${PROJECT_BINARY_DIR}/gisland-battery"
      "${Python3_EXECUTABLE}" -m unittest discover -s
      "${PROJECT_SOURCE_DIR}/tests/battery" -p "test_*.py" -v
  )
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

  add_executable(gisland_rlgl_renderer_model_tests tests/rlgl_renderer_model_test.cpp)
  target_compile_features(gisland_rlgl_renderer_model_tests PRIVATE cxx_std_23)
  target_link_libraries(
    gisland_rlgl_renderer_model_tests
    PRIVATE gisland::rlgl_renderer_model Catch2::Catch2WithMain
  )
  gisland_set_project_options(gisland_rlgl_renderer_model_tests)
  add_test(NAME rlgl_renderer_model COMMAND gisland_rlgl_renderer_model_tests)

  add_executable(gisland_rlgl_font_book_tests tests/rlgl_font_book_test.cpp)
  target_compile_features(gisland_rlgl_font_book_tests PRIVATE cxx_std_23)
  target_link_libraries(
    gisland_rlgl_font_book_tests
    PRIVATE gisland::rlgl_renderer_model gisland::app Catch2::Catch2WithMain
  )
  target_compile_definitions(
    gisland_rlgl_font_book_tests
    PRIVATE GISLAND_TEST_ASSET_ROOT="${PROJECT_BINARY_DIR}/assets"
  )
  add_dependencies(gisland_rlgl_font_book_tests gisland_assets)
  gisland_set_project_options(gisland_rlgl_font_book_tests)
  add_test(NAME rlgl_font_book COMMAND gisland_rlgl_font_book_tests)

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
    tests/display_test.cpp
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
    tests/platform_host_test.cpp
    tests/protocol_serialization_test.cpp
    tests/protocol_test.cpp
    tests/reload_test.cpp
    tests/rich_text_test.cpp
    tests/runtime_test.cpp
    tests/scene_test.cpp
    tests/scene_template_test.cpp
    tests/theme_test.cpp
    tests/write_queue_test.cpp
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
      GISLAND_LUA_HOST_PATH="$<TARGET_FILE:gisland-lua-host>"
      GISLAND_CLOCK_CALENDAR_PATH="${PROJECT_BINARY_DIR}/gisland-clock-calendar"
      GISLAND_TEST_ASSET_ROOT="${PROJECT_BINARY_DIR}/assets"
  )
  add_dependencies(gisland_tests fake_module gisland-lua-host gisland_clock_calendar gisland_assets)
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
    PRIVATE gisland::lua_runtime Catch2::Catch2WithMain
  )
  target_compile_definitions(
    gisland_lua_host_tests
    PRIVATE
      GISLAND_LUA_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures/lua"
      GISLAND_LUA_HOST_PATH="$<TARGET_FILE:gisland-lua-host>"
  )
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
    find_package(PNG REQUIRED)
    if(NOT TARGET PkgConfig::EGL)
      pkg_check_modules(EGL REQUIRED IMPORTED_TARGET egl)
    endif()
    add_executable(
      gisland_rlgl_gpu_tests
      tests/fixtures/primitive_gallery.cpp
      tests/rlgl_gpu_test.cpp
    )
    target_compile_features(gisland_rlgl_gpu_tests PRIVATE cxx_std_23)
    target_compile_definitions(gisland_rlgl_gpu_tests PRIVATE GRAPHICS_API_OPENGL_33)
    target_include_directories(
      gisland_rlgl_gpu_tests
      PRIVATE ${PROJECT_SOURCE_DIR}/include ${raylib_SOURCE_DIR}/src
    )
    target_link_libraries(
      gisland_rlgl_gpu_tests
      PRIVATE
        gisland::rlgl_gpu
        gisland::rich_text
        gisland::scene_layout
        gisland::theme
        Catch2::Catch2WithMain
        PNG::PNG
        PkgConfig::EGL
    )
    gisland_set_project_options(gisland_rlgl_gpu_tests)
    target_compile_definitions(
      gisland_rlgl_gpu_tests
      PRIVATE
        GISLAND_TEST_ASSET_ROOT="${PROJECT_BINARY_DIR}/assets"
        GISLAND_TEST_BASELINE_ROOT="${PROJECT_SOURCE_DIR}/tests/baselines"
        GISLAND_VISUAL_ARTIFACT_ROOT="${PROJECT_BINARY_DIR}/visual-artifacts"
    )
    add_dependencies(gisland_rlgl_gpu_tests gisland_assets)
    catch_discover_tests(
      gisland_rlgl_gpu_tests
      TEST_PREFIX "rlgl_gpu::"
      PROPERTIES
        LABELS graphical
        RUN_SERIAL true
        ENVIRONMENT "EGL_PLATFORM=surfaceless;LIBGL_ALWAYS_SOFTWARE=1"
      DISCOVERY_MODE PRE_TEST
    )

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
        GISLAND_CLOCK_CALENDAR_PATH="$<TARGET_FILE:gisland_clock_calendar>"
        GISLAND_FAKE_MODULE_PATH="$<TARGET_FILE:fake_module>"
        GISLAND_LUA_HOST_PATH="$<TARGET_FILE:gisland-lua-host>"
        GISLAND_AUDIO_LUA_PATH="${PROJECT_SOURCE_DIR}/assets/modules/audio/audio.lua"
        GISLAND_AUDIO_FAKE_PACTL_PATH="${PROJECT_SOURCE_DIR}/tests/audio/fixtures/fake-pactl"
        GISLAND_AUDIO_FAKE_GISLANDCTL_PATH="${PROJECT_SOURCE_DIR}/tests/audio/fixtures/fake-gislandctl"
        GISLAND_NOTIFICATION_HISTORY_PATH="${PROJECT_BINARY_DIR}/gisland-notification-history"
    )
    gisland_set_project_options(gisland_application_x11_smoke_tests)
    add_dependencies(
      gisland_application_x11_smoke_tests
      gisland
      gislandctl
      gisland-lua-host
      gisland_clock_calendar
      fake_module
    )
    catch_discover_tests(
      gisland_application_x11_smoke_tests
      TEST_PREFIX "application_x11::"
      PROPERTIES LABELS graphical RUN_SERIAL true
      DISCOVERY_MODE PRE_TEST
    )

    add_executable(gisland_x11_window_tests tests/x11_platform_host_test.cpp)
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
      PRIVATE gisland::app gisland::rlgl_renderer_model Catch2::Catch2WithMain raylib
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

    add_executable(
      gisland_visual_regression_tests
      tests/fixtures/primitive_gallery.cpp
      tests/visual_regression_test.cpp
    )
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
  include/gisland/clock_calendar.hpp
  include/gisland/content_fingerprint.hpp
  include/gisland/config.hpp
  include/gisland/control.hpp
  include/gisland/control_client.hpp
  include/gisland/control_dispatcher.hpp
  include/gisland/context.hpp
  include/gisland/display.hpp
  include/gisland/file_watcher.hpp
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
  include/gisland/platform_host.hpp
  include/gisland/process_backend.hpp
  include/gisland/protocol.hpp
  include/gisland/raylib_renderer.hpp
  include/gisland/rlgl_font_book.hpp
  include/gisland/rlgl_gpu.hpp
  include/gisland/rlgl_painter.hpp
  include/gisland/rlgl_renderer_model.hpp
  include/gisland/rlgl_texture_books.hpp
  include/gisland/reload.hpp
  include/gisland/rich_text.hpp
  include/gisland/runtime.hpp
  include/gisland/scene.hpp
  include/gisland/scene_template.hpp
  include/gisland/theme.hpp
  include/gisland/write_queue.hpp
  include/gisland/x11_platform_host.hpp
  include/gisland/x11_shape.hpp
  prototype/wayland/layer_shell_protocol.hpp
  prototype/wayland/main.cpp
  prototype/wayland/options.cpp
  prototype/wayland/runtime.cpp
  prototype/wayland/wayland_prototype_options.hpp
  prototype/wayland/wayland_prototype_runtime.hpp
  prototype/wayland/xdg_popup_interface.cpp
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
  src/display.cpp
  src/file_watcher.cpp
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
  src/posix_process_backend.cpp
  src/protocol.cpp
  src/raylib_renderer.cpp
  src/rlgl_font_book.cpp
  src/rlgl_gpu.cpp
  src/rlgl_impl.cpp
  src/rlgl_painter.cpp
  src/rlgl_renderer_model.cpp
  src/rlgl_texture_books.cpp
  src/reload.cpp
  src/rich_text.cpp
  src/runtime.cpp
  src/scene.cpp
  src/scene_template.cpp
  src/theme.cpp
  src/write_queue.cpp
  src/x11_platform_host.cpp
  src/x11_shape.cpp
  tests/fake_module.cpp
  tests/fixtures/primitive_gallery.cpp
  tests/fixtures/primitive_gallery.hpp
  tests/audio/audio_process.hpp
  tests/application_x11_smoke_test.cpp
  tests/application_config_test.cpp
  tests/bootstrap_test.cpp
  tests/clock_calendar_test.cpp
  tests/clock_calendar_process_test.cpp
  tests/config_test.cpp
  tests/control_client_test.cpp
  tests/control_dispatcher_test.cpp
  tests/control_test.cpp
  tests/display_test.cpp
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
  tests/platform_host_test.cpp
  tests/protocol_serialization_test.cpp
  tests/protocol_test.cpp
  tests/reload_test.cpp
  tests/rich_text_test.cpp
  tests/runtime_test.cpp
  tests/raylib_renderer_test.cpp
  tests/raylib_gallery_reference.cpp
  tests/rlgl_font_book_test.cpp
  tests/rlgl_gpu_test.cpp
  tests/rlgl_x11_gallery_reference.cpp
  tests/rlgl_renderer_model_test.cpp
  tests/scene_test.cpp
  tests/scene_template_test.cpp
  tests/theme_test.cpp
  tests/visual_regression_test.cpp
  tests/wayland_prototype_options_test.cpp
  tests/write_queue_test.cpp
   tests/x11_platform_host_test.cpp
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
