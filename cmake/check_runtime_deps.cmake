# check_runtime_deps.cmake
#
# Fail if anything in a finished tree needs a DLL that is not beside it and not
# part of Windows.
#
# This replaces "launch it and see" as the release gate. Launching cannot detect
# the failure it is meant to catch: a missing DLL puts up a modal
# "The code execution cannot proceed because X was not found" dialog, and a
# process showing that dialog has *not* exited -- so a HasExited check reports
# success while the dialog waits for a click that nobody is there to give. It
# also wedges an unattended build.
#
# Resolving the import graph instead is deterministic, needs no GUI, and names
# the missing file.
#
#   cmake -DBIN_DIR=<dir> -DEXECUTABLES=a.exe;b.exe -P check_runtime_deps.cmake

if(NOT DEFINED BIN_DIR)
    message(FATAL_ERROR "BIN_DIR is required")
endif()

file(GLOB _crd_dlls "${BIN_DIR}/*.dll")

set(_crd_exes "")
foreach(_crd_e IN LISTS EXECUTABLES)
    if(EXISTS "${BIN_DIR}/${_crd_e}")
        list(APPEND _crd_exes "${BIN_DIR}/${_crd_e}")
    endif()
endforeach()

if(NOT _crd_exes)
    message(FATAL_ERROR "no executables found in ${BIN_DIR}")
endif()

# DIRECTORIES is deliberately only the tree itself: anything that has to be
# found elsewhere is exactly what would be missing on someone else's machine.
file(GET_RUNTIME_DEPENDENCIES
     EXECUTABLES                 ${_crd_exes}
     LIBRARIES                   ${_crd_dlls}
     RESOLVED_DEPENDENCIES_VAR   _crd_resolved
     UNRESOLVED_DEPENDENCIES_VAR _crd_unresolved
     DIRECTORIES                 "${BIN_DIR}"
     PRE_EXCLUDE_REGEXES  "api-ms-.*" "ext-ms-.*"
     POST_EXCLUDE_REGEXES ".*[/\\\\][Ss]ystem32[/\\\\].*" ".*[/\\\\][Ww]inSxS[/\\\\].*")

# Anything resolved from outside the tree is a dependency that travels only on
# this machine, so treat it the same as unresolved.
file(TO_CMAKE_PATH "${BIN_DIR}" _crd_bin)
set(_crd_outside "")
foreach(_crd_dep IN LISTS _crd_resolved)
    get_filename_component(_crd_dir "${_crd_dep}" DIRECTORY)
    file(TO_CMAKE_PATH "${_crd_dir}" _crd_dir)
    if(NOT _crd_dir STREQUAL _crd_bin)
        list(APPEND _crd_outside "${_crd_dep}")
    endif()
endforeach()

set(_crd_bad "")
foreach(_crd_u IN LISTS _crd_unresolved)
    # API-set stubs resolve to no file on some Windows versions even when the
    # pre-filters are right; they are always present at run time.
    if(NOT _crd_u MATCHES "^(api-ms-|ext-ms-)")
        list(APPEND _crd_bad "${_crd_u}")
    endif()
endforeach()

if(_crd_bad OR _crd_outside)
    message("")
    foreach(_crd_u IN LISTS _crd_bad)
        message("  MISSING: ${_crd_u}")
    endforeach()
    foreach(_crd_o IN LISTS _crd_outside)
        message("  OUTSIDE THE TREE: ${_crd_o}")
    endforeach()
    message(FATAL_ERROR
        "${BIN_DIR} depends on libraries it does not ship. It will start here, "
        "where those happen to be on PATH, and nowhere else.")
endif()

list(LENGTH _crd_dlls _crd_n)
message(STATUS "All imports resolve within the tree (${_crd_n} bundled DLLs)")
