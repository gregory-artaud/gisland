if(NOT DEFINED SOURCE_DIR OR NOT DEFINED BINARY_DIR OR NOT DEFINED SOURCE_PREFIX)
  message(FATAL_ERROR "SOURCE_DIR, BINARY_DIR, and SOURCE_PREFIX are required")
endif()

foreach(layout IN ITEMS lib64 multiarch)
  set(root "${BINARY_DIR}/${layout}/root")
  set(build "${BINARY_DIR}/${layout}/build")
  file(REMOVE_RECURSE "${BINARY_DIR}/${layout}")
  file(MAKE_DIRECTORY "${root}")
  file(COPY "${SOURCE_PREFIX}/" DESTINATION "${root}")
  set(native_lua_dir "")
  foreach(candidate IN ITEMS "${root}/lib/lua" "${root}/lib64/lua")
    if(IS_DIRECTORY "${candidate}/5.4")
      set(native_lua_dir "${candidate}")
      break()
    endif()
  endforeach()
  if(NOT native_lua_dir)
    file(GLOB multiarch_candidates LIST_DIRECTORIES true "${root}/lib/*/lua")
    foreach(candidate IN LISTS multiarch_candidates)
      if(IS_DIRECTORY "${candidate}/5.4")
        set(native_lua_dir "${candidate}")
        break()
      endif()
    endforeach()
  endif()
  if(NOT native_lua_dir)
    message(FATAL_ERROR "SOURCE_PREFIX does not contain a staged Lua native module")
  endif()

  if(layout STREQUAL "lib64")
    file(MAKE_DIRECTORY "${root}/lib64")
    set(destination "${root}/lib64/lua")
  else()
    file(MAKE_DIRECTORY "${root}/lib/test-multiarch")
    set(destination "${root}/lib/test-multiarch/lua")
  endif()
  if(NOT "${native_lua_dir}" STREQUAL "${destination}")
    file(REMOVE_RECURSE "${destination}")
    file(RENAME "${native_lua_dir}" "${destination}")
  endif()

  execute_process(
    COMMAND
      "${CMAKE_COMMAND}" -S "${SOURCE_DIR}" -B "${build}" -G Ninja
      -DBUILD_TESTING=OFF -DGISLAND_LGI_ROOT=${root}
      -DFETCHCONTENT_SOURCE_DIR_RAYLIB=${RAYLIB_SOURCE_DIR}
      -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=${NLOHMANN_JSON_SOURCE_DIR}
      -DFETCHCONTENT_SOURCE_DIR_TOMLPLUSPLUS=${TOMLPLUSPLUS_SOURCE_DIR}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors
  )
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "${layout} staged lgi layout was rejected:\n${output}\n${errors}")
  endif()
endforeach()

set(probe_root "${BINARY_DIR}/probe/root")
file(REMOVE_RECURSE "${BINARY_DIR}/probe")
file(MAKE_DIRECTORY "${probe_root}")
file(COPY "${SOURCE_PREFIX}/" DESTINATION "${probe_root}")

set(mock_lgi [[
local namespaces = {
  GLib = {},
  Gio = {},
  Json = { version = "1.0" },
  GdkPixbuf = { version = "2.0" },
  Rsvg = { version = "2.0" },
  Gtk = { version = "3.0" },
}

local lgi = {
  GLib = namespaces.GLib,
  Gio = namespaces.Gio,
}

function lgi.require(namespace, version)
  local value = assert(namespaces[namespace], "mock namespace unavailable: " .. namespace)
  assert(value.version == version, "mock namespace version mismatch: " .. namespace)
  return value
end

return lgi
]])
file(WRITE "${probe_root}/share/lua/5.4/lgi.lua" "${mock_lgi}")

execute_process(
  COMMAND
    "${CMAKE_COMMAND}" -S "${SOURCE_DIR}" -B "${BINARY_DIR}/probe/missing-build" -G Ninja
    -DBUILD_TESTING=OFF -DGISLAND_LGI_ROOT=${probe_root}
    -DFETCHCONTENT_SOURCE_DIR_RAYLIB=${RAYLIB_SOURCE_DIR}
    -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=${NLOHMANN_JSON_SOURCE_DIR}
    -DFETCHCONTENT_SOURCE_DIR_TOMLPLUSPLUS=${TOMLPLUSPLUS_SOURCE_DIR}
  RESULT_VARIABLE missing_result
  OUTPUT_VARIABLE missing_output
  ERROR_VARIABLE missing_errors
)
if(missing_result EQUAL 0)
  message(FATAL_ERROR "staged lgi probe accepted a fixture without GioUnix")
endif()

string(REPLACE "  Gtk = { version = \"3.0\" },"
               "  Gtk = { version = \"3.0\" },\n  GioUnix = { version = \"2.0\" },"
               complete_mock_lgi "${mock_lgi}")
file(WRITE "${probe_root}/share/lua/5.4/lgi.lua" "${complete_mock_lgi}")
execute_process(
  COMMAND
    "${CMAKE_COMMAND}" -S "${SOURCE_DIR}" -B "${BINARY_DIR}/probe/complete-build" -G Ninja
    -DBUILD_TESTING=OFF -DGISLAND_LGI_ROOT=${probe_root}
    -DFETCHCONTENT_SOURCE_DIR_RAYLIB=${RAYLIB_SOURCE_DIR}
    -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=${NLOHMANN_JSON_SOURCE_DIR}
    -DFETCHCONTENT_SOURCE_DIR_TOMLPLUSPLUS=${TOMLPLUSPLUS_SOURCE_DIR}
  RESULT_VARIABLE complete_result
  OUTPUT_VARIABLE complete_output
  ERROR_VARIABLE complete_errors
)
if(NOT complete_result EQUAL 0)
  message(FATAL_ERROR
          "staged lgi probe rejected a complete fixture:\n${complete_output}\n${complete_errors}")
endif()
