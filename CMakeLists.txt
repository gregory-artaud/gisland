cmake_minimum_required(VERSION 3.28)

project(gisland VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "gisland currently supports Linux/X11 only")
endif()

include(CTest)
include(FetchContent)
include(cmake/ProjectOptions.cmake)

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

add_library(
  gisland_app STATIC
  src/application.cpp
  src/config.cpp
  src/context.cpp
  src/protocol.cpp
  src/scene.cpp
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
)
gisland_set_project_options(gisland_app)

add_executable(gisland src/main.cpp)
target_compile_features(gisland PRIVATE cxx_std_23)
target_link_libraries(gisland PRIVATE gisland::app)
gisland_set_project_options(gisland)

if(BUILD_TESTING)
  add_executable(
    gisland_tests
    tests/application_config_test.cpp
    tests/config_test.cpp
    tests/context_test.cpp
    tests/protocol_serialization_test.cpp
    tests/protocol_test.cpp
    tests/scene_test.cpp
  )
  target_compile_features(gisland_tests PRIVATE cxx_std_23)
  target_link_libraries(gisland_tests PRIVATE gisland::app Catch2::Catch2WithMain)
  gisland_set_project_options(gisland_tests)
  list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
  include(Catch)
  catch_discover_tests(gisland_tests DISCOVERY_MODE PRE_TEST)
endif()

set(
  GISLAND_FORMAT_SOURCES
  include/gisland/application.hpp
  include/gisland/config.hpp
  include/gisland/context.hpp
  include/gisland/protocol.hpp
  include/gisland/scene.hpp
  src/application.cpp
  src/config.cpp
  src/context.cpp
  src/main.cpp
  src/protocol.cpp
  src/scene.cpp
  tests/application_config_test.cpp
  tests/config_test.cpp
  tests/context_test.cpp
  tests/protocol_serialization_test.cpp
  tests/protocol_test.cpp
  tests/scene_test.cpp
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
