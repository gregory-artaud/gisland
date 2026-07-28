include_guard(GLOBAL)

option(GISLAND_WARNINGS_AS_ERRORS "Treat project warnings as errors" OFF)
option(GISLAND_ENABLE_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
option(GISLAND_ENABLE_CLANG_TIDY "Run clang-tidy while compiling project targets" OFF)

function(gisland_set_project_options target)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(
      ${target}
      PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wshadow
        -Wnon-virtual-dtor
    )
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(
      ${target}
      PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wshadow
        -Wnon-virtual-dtor
    )
  else()
    message(FATAL_ERROR "gisland supports GCC and Clang on Linux")
  endif()

  if(GISLAND_WARNINGS_AS_ERRORS)
    target_compile_options(${target} PRIVATE -Werror)
  endif()

  if(GISLAND_ENABLE_SANITIZERS)
    target_compile_options(
      ${target}
      PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer
    )
    target_link_options(
      ${target}
      PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer
    )
  endif()

  if(GISLAND_ENABLE_CLANG_TIDY)
    find_program(CLANG_TIDY_EXECUTABLE NAMES clang-tidy REQUIRED)
    set_target_properties(
      ${target}
      PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_EXECUTABLE}"
    )
  endif()
endfunction()
