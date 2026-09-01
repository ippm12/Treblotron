# bundle_runtime_deps.cmake
#
# Copy toolchain DLLs that the DLLs in a build tree depend on.
#
# $<TARGET_RUNTIME_DLLS> covers what our own targets link, but it does not
# recurse: it copies SDL3_ttf.dll and stops, while SDL3_ttf.dll itself imports
# libwinpthread-1.dll from MSYS2. On a machine with MSYS2 on PATH nothing looks
# wrong, and anywhere else the process dies before main() with
#
#     The code execution cannot proceed because libwinpthread-1.dll was not found
#
# The installed tree already gets this treatment from install_runtime_deps.cmake.
# Doing it here as well means the build tree runs standalone too, which is what
# scripts/release.ps1 checks and what anyone running from build/bin expects.
#
# Invoked per target as a POST_BUILD step:
#   cmake -DBIN_DIR=... -DEXECUTABLE=... -DTOOLCHAIN_BIN=... -P bundle_runtime_deps.cmake

if(NOT DEFINED BIN_DIR OR NOT DEFINED EXECUTABLE)
    message(FATAL_ERROR "BIN_DIR and EXECUTABLE are required")
endif()

if(NOT EXISTS "${EXECUTABLE}")
    return()
endif()

file(GLOB _brd_dlls "${BIN_DIR}/*.dll")

file(GET_RUNTIME_DEPENDENCIES
     EXECUTABLES                 "${EXECUTABLE}"
     LIBRARIES                   ${_brd_dlls}
     RESOLVED_DEPENDENCIES_VAR   _brd_resolved
     UNRESOLVED_DEPENDENCIES_VAR _brd_unresolved
     DIRECTORIES                 "${BIN_DIR}" "${TOOLCHAIN_BIN}"
     # Windows' own DLLs are present on every target and are not ours to ship.
     # The api-ms-/ext-ms- patterns are API-set stubs that resolve to no file.
     PRE_EXCLUDE_REGEXES  "api-ms-.*" "ext-ms-.*"
     POST_EXCLUDE_REGEXES ".*[/\\\\][Ss]ystem32[/\\\\].*" ".*[/\\\\][Ww]inSxS[/\\\\].*")

file(TO_CMAKE_PATH "${BIN_DIR}" _brd_bin)

foreach(_brd_dep IN LISTS _brd_resolved)
    get_filename_component(_brd_dir "${_brd_dep}" DIRECTORY)
    file(TO_CMAKE_PATH "${_brd_dir}" _brd_dir)
    if(_brd_dir STREQUAL _brd_bin)
        continue()                      # already here; do not copy onto itself
    endif()
    get_filename_component(_brd_name "${_brd_dep}" NAME)
    message(STATUS "Bundling runtime dependency: ${_brd_name}")
    file(COPY "${_brd_dep}" DESTINATION "${_brd_bin}")
endforeach()
