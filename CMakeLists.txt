cmake_minimum_required(VERSION 3.22.1)

# CMAKE_CXX_STANDARD set as a directory variable would leak into OpenCV /
# SDL subprojects and interact badly with their own compiler-feature
# checks. We apply cxx_std_23 to our targets explicitly via
# target_compile_features() below instead.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(PRJ_NAME Dartmatic)
set(SERVER_NAME Dartmatic_Server)

# ============================================================================
# Configuration
# ============================================================================
#
# Two independent axes:
#
#   DARTMATIC_VISION_SOURCE  where the game gets dart events from
#   DARTMATIC_INFER_BACKEND  what actually executes a forward pass
#
# They're separate because the same inference backend serves two very
# different binaries: the game running detection locally, and the headless
# server running it on behalf of a remote client. Typical combinations:
#
#   dev workstation      VISION_SOURCE=sim      INFER_BACKEND=none
#   Jetson, standalone   VISION_SOURCE=local    INFER_BACKEND=tensorrt
#   Raspberry Pi + server VISION_SOURCE=network INFER_BACKEND=none
#   inference server     BUILD_APP=OFF BUILD_SERVER=ON INFER_BACKEND=cpu|tensorrt

option(DARTMATIC_BUILD_APP    "Build the Dartmatic game application"    ON)
option(DARTMATIC_BUILD_SERVER "Build the headless inference server"     OFF)
option(DARTMATIC_SHOW_FPS     "Show framerate counter in the top right" ON)

set(DARTMATIC_VISION_SOURCE "sim"  CACHE STRING "Vision source: sim|local|network")
set(DARTMATIC_INFER_BACKEND "none" CACHE STRING "Inference backend: none|tensorrt|directml|cpu")
set_property(CACHE DARTMATIC_VISION_SOURCE PROPERTY STRINGS sim local network)
set_property(CACHE DARTMATIC_INFER_BACKEND PROPERTY STRINGS none tensorrt directml cpu)

# Pinned so a silent upstream bump can't change inference behaviour underfoot.
set(DARTMATIC_ORT_VERSION "1.24.4" CACHE STRING "ONNX Runtime version for the directml backend")
set(DARTMATIC_DIRECTML_VERSION "1.15.4" CACHE STRING "DirectML redistributable version")

# Windows ships a DirectML.dll in System32, and by default we use it — that
# needs no download and adds no redistribution obligation, since it is part of
# the user's Windows licence. The catch is that the inbox copy tracks the OS
# version: an older Windows 10 can carry a DirectML too old for the pinned
# ONNX Runtime, in which case the backend reports itself as
# "ONNX Runtime (CPU — DirectML unavailable)" and runs ~13x slower.
#
# Turn this on to fetch and ship the redistributable instead. It costs a
# ~200 MB one-time download (the package carries every architecture) for one
# 18 MB DLL, which is why it is not the default.
option(DARTMATIC_FETCH_DIRECTML "Ship the DirectML redistributable instead of using the Windows copy" OFF)

# Back-compat with the previous boolean flags so existing command lines,
# .vscode configs and build caches keep working.
#
# Two rules keep a legacy flag from outliving its usefulness:
#
#   - it only applies when the new variable is still at its default, so an
#     explicit -DDARTMATIC_VISION_SOURCE always wins;
#   - it is removed from the cache once honoured, so it cannot silently steer
#     every future configure of the same build directory.
#
# Both matter because the old flags live in existing build caches. Without
# them, reconfiguring a build directory that once had DARTMATIC_USE_HAILO=ON
# would keep demanding HailoRT no matter what you asked for — which is exactly
# what happens when a Pi's accelerator is removed and the build tree isn't.
# That flag is now gone entirely, but it is still cleared below, because the
# stale caches that made it a problem are still out there.
macro(_dartmatic_retire_flag legacy explanation)
    if(${legacy})
        if(_dartmatic_source_is_default)
            message(DEPRECATION "${legacy} is deprecated — ${explanation}")
            set(_dartmatic_legacy_applied TRUE)
        else()
            message(WARNING
                "Ignoring stale ${legacy} from the build cache: "
                "DARTMATIC_VISION_SOURCE=${DARTMATIC_VISION_SOURCE} was requested explicitly. "
                "Clearing ${legacy} so it stops interfering.")
        endif()
    endif()
    unset(${legacy} CACHE)
endmacro()

set(_dartmatic_source_is_default FALSE)
if(DARTMATIC_VISION_SOURCE STREQUAL "sim")
    set(_dartmatic_source_is_default TRUE)
endif()

# Hailo support was removed in v0.1.0: the accelerator path targeted a model
# generation two contracts old, ran its own detection pipeline rather than the
# shared one, and loaded a .hef that was never in this repository. Say so
# outright rather than silently ignoring the flag — someone reconfiguring an old
# build tree deserves to know the option went away rather than wonder why their
# setting had no effect.
if(DARTMATIC_USE_HAILO)
    message(WARNING
        "DARTMATIC_USE_HAILO is set, but Hailo support was removed in v0.1.0. "
        "Clearing it. Use -DDARTMATIC_VISION_SOURCE=network for a Pi that streams "
        "to an inference server, or =local to run the models on this machine.")
endif()
unset(DARTMATIC_USE_HAILO CACHE)

set(_dartmatic_legacy_applied FALSE)
_dartmatic_retire_flag(DARTMATIC_USE_TENSORRT
    "use -DDARTMATIC_VISION_SOURCE=local -DDARTMATIC_INFER_BACKEND=tensorrt")
if(_dartmatic_legacy_applied)
    set(DARTMATIC_VISION_SOURCE "local"    CACHE STRING "Vision source"      FORCE)
    set(DARTMATIC_INFER_BACKEND "tensorrt" CACHE STRING "Inference backend"  FORCE)
endif()

# Never read; retired so it cannot confuse anyone reading a stale cache.
unset(DARTMATIC_USE_SIM CACHE)

if(NOT DARTMATIC_VISION_SOURCE MATCHES "^(sim|local|network)$")
    message(FATAL_ERROR "DARTMATIC_VISION_SOURCE must be sim, local or network "
                        "(got '${DARTMATIC_VISION_SOURCE}')")
endif()
if(NOT DARTMATIC_INFER_BACKEND MATCHES "^(none|tensorrt|directml|cpu)$")
    message(FATAL_ERROR "DARTMATIC_INFER_BACKEND must be none, tensorrt, directml or cpu "
                        "(got '${DARTMATIC_INFER_BACKEND}')")
endif()
if(DARTMATIC_INFER_BACKEND STREQUAL "directml" AND NOT WIN32)
    message(FATAL_ERROR "The directml backend is Windows-only (DirectML is a DirectX 12 "
                        "component). Use tensorrt on Jetson, or cpu elsewhere.")
endif()
if(NOT DARTMATIC_BUILD_APP AND NOT DARTMATIC_BUILD_SERVER)
    message(FATAL_ERROR "Nothing to build — enable DARTMATIC_BUILD_APP and/or DARTMATIC_BUILD_SERVER")
endif()

# The pieces that need a real backend: the local vision source, and the server.
set(_dartmatic_needs_backend OFF)
if(DARTMATIC_BUILD_APP AND DARTMATIC_VISION_SOURCE STREQUAL "local")
    set(_dartmatic_needs_backend ON)
endif()
if(DARTMATIC_BUILD_SERVER)
    set(_dartmatic_needs_backend ON)
endif()
if(_dartmatic_needs_backend AND DARTMATIC_INFER_BACKEND STREQUAL "none")
    message(FATAL_ERROR
        "This configuration runs the detection models, so it needs an inference "
        "backend. Add -DDARTMATIC_INFER_BACKEND=tensorrt (Jetson) or =cpu (OpenCV DNN).")
endif()

# CUDA has to be declared in project(LANGUAGES ...) — more robust on
# CMake 3.22 / JetPack than enable_language(CUDA) after project().
set(_DARTMATIC_LANGUAGES C CXX)
if(DARTMATIC_INFER_BACKEND STREQUAL "tensorrt")
    if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
        # Orin Nano Super: Ampere, compute capability 8.7.
        set(CMAKE_CUDA_ARCHITECTURES 87)
    endif()
    list(APPEND _DARTMATIC_LANGUAGES CUDA)
endif()

project(${PRJ_NAME}
        VERSION   0.1.0
        LANGUAGES ${_DARTMATIC_LANGUAGES})

# Surfaced in the startup log, on the main menu and via --version, so a bug
# report can name the build it came from. Generated rather than hand-written in
# a header, so the number lives in exactly one place.
configure_file(
    "${PROJECT_SOURCE_DIR}/inc_common/dartmatic_version.hpp.in"
    "${PROJECT_BINARY_DIR}/generated/dartmatic_version.hpp"
    @ONLY)

message(STATUS "Dartmatic ${PROJECT_VERSION}: app=${DARTMATIC_BUILD_APP} "
               "server=${DARTMATIC_BUILD_SERVER} "
               "vision_source=${DARTMATIC_VISION_SOURCE} backend=${DARTMATIC_INFER_BACKEND}")


# ============================================================================
# Module helper
# ============================================================================
#
# A module is a directory with public headers in <mod>/inc_public/<mod>/ and
# flat implementation files in <mod>/*.cpp. Globbed with CONFIGURE_DEPENDS so
# there are no source lists to maintain. Two targets share this, which is why
# it's a function rather than the inline loop it used to be.

function(dartmatic_add_module target module)
    target_include_directories(${target} PUBLIC ${PROJECT_SOURCE_DIR}/${module}/inc_public)
    file(GLOB LIB_SOURCES CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/${module}/*.h"
        "${PROJECT_SOURCE_DIR}/${module}/*.hpp"
        "${PROJECT_SOURCE_DIR}/${module}/*.cpp")
    file(GLOB_RECURSE LIB_PUBLIC_SOURCES CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/${module}/inc_public/${module}/*.h"
        "${PROJECT_SOURCE_DIR}/${module}/inc_public/${module}/*.hpp")
    target_sources(${target} PRIVATE ${LIB_PUBLIC_SOURCES} ${LIB_SOURCES})
endfunction()

# Shared settings every Dartmatic target wants.
function(dartmatic_configure_target target)
    target_compile_features(${target} PRIVATE cxx_std_23)
    target_compile_options(${target} PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Werror>
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
    )
    # Common include should be available to everyone
    target_include_directories(${target} PUBLIC ${PROJECT_SOURCE_DIR}/inc_common)
    # ...as should the generated version header.
    target_include_directories(${target} PUBLIC ${PROJECT_BINARY_DIR}/generated)
    file(GLOB INC_COMMON_SOURCES CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/inc_common/*.h"
        "${PROJECT_SOURCE_DIR}/inc_common/*.hpp"
        "${PROJECT_SOURCE_DIR}/inc_common/*.cpp")
    target_sources(${target} PRIVATE ${INC_COMMON_SOURCES})

    if(DARTMATIC_INFER_BACKEND STREQUAL "cpu")
        target_compile_definitions(${target} PRIVATE DARTMATIC_INFER_CPU)
    elseif(DARTMATIC_INFER_BACKEND STREQUAL "tensorrt")
        target_compile_definitions(${target} PRIVATE DARTMATIC_INFER_TENSORRT)
    elseif(DARTMATIC_INFER_BACKEND STREQUAL "directml")
        target_compile_definitions(${target} PRIVATE DARTMATIC_INFER_DIRECTML)
    endif()

    # Carry the GCC runtime inside the executable.
    #
    # Without this, a MinGW build imports libstdc++-6.dll, libgcc_s_seh-1.dll
    # and libwinpthread-1.dll from wherever MSYS2 happens to be installed. On
    # the machine that built it those sit on PATH and everything works; on any
    # other machine the process dies before main() with a missing-DLL dialog.
    # It is the kind of bug you cannot find by testing locally, which is
    # exactly why it is pinned here rather than papered over by copying DLLs
    # next to the exe.
    #
    # winpthread needs the whole-archive treatment: parts of it are pulled in
    # by libstdc++'s threading support rather than referenced directly, so a
    # plain -Bstatic leaves the import behind.
    if(MINGW)
        target_link_options(${target} PRIVATE
            -static-libgcc -static-libstdc++
            -Wl,-Bstatic,--whole-archive -lwinpthread
            -Wl,--no-whole-archive,-Bdynamic)
    endif()

    # Strip symbols from release builds. A MinGW debug binary here is 182 MB
    # against 12 MB stripped, almost all of it DWARF that nobody receiving an
    # installer can act on. Diagnosis for a released build comes from the logs,
    # which name the version and say where they are.
    target_link_options(${target} PRIVATE
        $<$<AND:$<CONFIG:Release>,$<BOOL:${MINGW}>>:-s>)
endfunction()


# ============================================================================
# Source-file selection
# ============================================================================
#
# set_source_files_properties is directory-scoped, so these exclusions apply to
# every target in this directory. That's fine: which vision source and which
# backend are configuration-wide choices, not per-target ones.

# Exactly one vision source compiles.
set(_all_vision_sources
    ${PROJECT_SOURCE_DIR}/vision/sim_vision_source.cpp
    ${PROJECT_SOURCE_DIR}/vision/local_vision_source.cpp
    ${PROJECT_SOURCE_DIR}/vision/network_vision_source.cpp)
set(_active_vision_source
    ${PROJECT_SOURCE_DIR}/vision/${DARTMATIC_VISION_SOURCE}_vision_source.cpp)
foreach(_src IN LISTS _all_vision_sources)
    if(NOT _src STREQUAL _active_vision_source OR NOT DARTMATIC_BUILD_APP)
        set_source_files_properties(${_src} PROPERTIES HEADER_FILE_ONLY TRUE)
    endif()
endforeach()

# Exactly one inference backend compiles, and dart_detector.cpp only when
# there is a backend for it to call.
set(_all_backends
    ${PROJECT_SOURCE_DIR}/detect/cpu_backend.cpp
    ${PROJECT_SOURCE_DIR}/detect/trt_backend.cpp
    ${PROJECT_SOURCE_DIR}/detect/ort_backend.cpp)
set(_backend_source_cpu      ${PROJECT_SOURCE_DIR}/detect/cpu_backend.cpp)
set(_backend_source_tensorrt ${PROJECT_SOURCE_DIR}/detect/trt_backend.cpp)
set(_backend_source_directml ${PROJECT_SOURCE_DIR}/detect/ort_backend.cpp)

foreach(_src IN LISTS _all_backends)
    if(NOT _src STREQUAL "${_backend_source_${DARTMATIC_INFER_BACKEND}}")
        set_source_files_properties(${_src} PROPERTIES HEADER_FILE_ONLY TRUE)
    endif()
endforeach()
if(DARTMATIC_INFER_BACKEND STREQUAL "none")
    set_source_files_properties(${PROJECT_SOURCE_DIR}/detect/dart_detector.cpp
        PROPERTIES HEADER_FILE_ONLY TRUE)
endif()


# ============================================================================
# Game application
# ============================================================================

if(DARTMATIC_BUILD_APP)
    # ---- Windows executable icon --------------------------------------
    #
    # Without an embedded icon the shipped exe shows the generic MinGW
    # placeholder in Explorer, the taskbar and Alt-Tab -- the installer icon
    # does not cover any of those. windres compiles the .rc; the path is
    # configured in rather than hard-coded so the build tree stays relocatable.
    set(_dm_app_sources startup.cpp)
    if(WIN32)
        enable_language(RC)
        set(DARTMATIC_ICON_PATH "${PROJECT_SOURCE_DIR}/assets/branding/icon.ico")
        # windres reads the path out of a C string, so backslashes would be
        # escapes. Forward slashes work and avoid the whole question.
        string(REPLACE "\\" "/" DARTMATIC_ICON_PATH "${DARTMATIC_ICON_PATH}")
        configure_file("${PROJECT_SOURCE_DIR}/packaging/windows/app.rc.in"
                       "${PROJECT_BINARY_DIR}/app.rc" @ONLY)
        list(APPEND _dm_app_sources "${PROJECT_BINARY_DIR}/app.rc")
    endif()

    add_executable(${PRJ_NAME} ${_dm_app_sources})
    dartmatic_configure_target(${PRJ_NAME})

    set(APP_MODULES_LIST "debug" "frame" "dart" "players" "game_lib" "games"
                         "detect" "net" "vision")
    foreach(lib IN LISTS APP_MODULES_LIST)
        message("Including ${lib} library")
        dartmatic_add_module(${PRJ_NAME} ${lib})
    endforeach()

    # Auto-discover game subdirectories (each game is a self-contained folder)
    file(GLOB GAME_SUBDIRS LIST_DIRECTORIES true CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/games/*/")
    foreach(game_dir IN LISTS GAME_SUBDIRS)
        if(IS_DIRECTORY ${game_dir})
            get_filename_component(game_name ${game_dir} NAME)
            # Skip inc_public — it's handled by the module loop above
            if(NOT game_name STREQUAL "inc_public")
                message("  Including game: ${game_name}")
                file(GLOB GAME_SRCS CONFIGURE_DEPENDS
                    "${game_dir}/*.h" "${game_dir}/*.hpp" "${game_dir}/*.cpp")
                target_sources(${PRJ_NAME} PRIVATE ${GAME_SRCS})
                target_include_directories(${PRJ_NAME} PRIVATE ${game_dir})
                # Copy per-game assets if they exist
                if(EXISTS "${game_dir}/assets")
                    add_custom_command(TARGET ${PRJ_NAME} POST_BUILD
                        COMMAND ${CMAKE_COMMAND} -E copy_directory
                        ${game_dir}/assets
                        $<TARGET_FILE_DIR:${PRJ_NAME}>/assets/games/${game_name}
                    )
                endif()
            endif()
        endif()
    endforeach()

    string(TOUPPER ${DARTMATIC_VISION_SOURCE} _vision_source_upper)
    target_compile_definitions(${PRJ_NAME} PRIVATE DARTMATIC_USE_${_vision_source_upper})

    if(DARTMATIC_VISION_SOURCE STREQUAL "local")
        # The capture threads pre-compute seg input and the detector publishes
        # its own warped frames — see camera_api.cpp.
        target_compile_definitions(${PRJ_NAME} PRIVATE DARTMATIC_HAVE_LOCAL_INFERENCE)
    endif()

    if(DARTMATIC_VISION_SOURCE STREQUAL "network")
        # Forward the cameras' own MJPEG instead of decoding and re-encoding it.
        target_compile_definitions(${PRJ_NAME} PRIVATE DARTMATIC_PASSTHROUGH_CAPTURE)
    endif()

    if(DARTMATIC_SHOW_FPS)
        target_compile_definitions(${PRJ_NAME} PRIVATE DARTMATIC_SHOW_FPS)
    endif()

    # Copy assets to build output directory
    add_custom_command(TARGET ${PRJ_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${PROJECT_SOURCE_DIR}/assets
        $<TARGET_FILE_DIR:${PRJ_NAME}>/assets
    )
endif()


# ============================================================================
# Inference server
# ============================================================================
#
# Deliberately spare: no SDL, no flecs, no game_lib, no games, no vision.
# Just enough to decode frames, run the models, and answer a client.

if(DARTMATIC_BUILD_SERVER)
    add_executable(${SERVER_NAME} server/server_main.cpp)
    dartmatic_configure_target(${SERVER_NAME})

    set(SERVER_MODULES_LIST "debug" "dart" "detect" "net" "server")
    foreach(lib IN LISTS SERVER_MODULES_LIST)
        message("Including ${lib} library (server)")
        dartmatic_add_module(${SERVER_NAME} ${lib})
    endforeach()
endif()


# ============================================================================
# Backend dependencies
# ============================================================================

set(DARTMATIC_TARGETS "")
if(DARTMATIC_BUILD_APP)
    list(APPEND DARTMATIC_TARGETS ${PRJ_NAME})
endif()
if(DARTMATIC_BUILD_SERVER)
    list(APPEND DARTMATIC_TARGETS ${SERVER_NAME})
endif()

# Winsock. Both targets link the net module — the server always, the app
# whenever the network vision source is in play — and ws2_32 is harmless
# otherwise.
if(WIN32)
    foreach(_t IN LISTS DARTMATIC_TARGETS)
        target_link_libraries(${_t} PRIVATE ws2_32)
    endforeach()
endif()

if(DARTMATIC_INFER_BACKEND STREQUAL "directml")
    # ONNX Runtime ships as MSVC-built binaries, but that is not a problem
    # here: ort_backend.cpp reaches the library through GetProcAddress on the
    # single exported C function OrtGetApiBase, so there is no link-time
    # dependency and no C++ ABI to match. We only need the headers at build
    # time and the DLLs next to the executable at run time.
    #
    # DirectML.dll itself is NOT fetched — Windows ships one in System32 (1.15.5
    # on this machine, newer than the redistributable), and the loader finds it
    # automatically. Point DARTMATIC_DIRECTML_DLL at a copy from the
    # Microsoft.AI.DirectML package if a target machine's inbox version is too
    # old for the pinned ONNX Runtime.
    set(_ort_root "${PROJECT_SOURCE_DIR}/external_libs/onnxruntime")
    set(_ort_stamp "${_ort_root}/.version-${DARTMATIC_ORT_VERSION}")

    if(NOT EXISTS "${_ort_stamp}")
        set(_ort_url "https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime.directml/${DARTMATIC_ORT_VERSION}/microsoft.ml.onnxruntime.directml.${DARTMATIC_ORT_VERSION}.nupkg")
        set(_ort_zip "${CMAKE_BINARY_DIR}/ort.nupkg")
        message(STATUS "Fetching ONNX Runtime ${DARTMATIC_ORT_VERSION} (DirectML) — ~12 MB, once")
        file(DOWNLOAD "${_ort_url}" "${_ort_zip}" STATUS _ort_dl SHOW_PROGRESS)
        list(GET _ort_dl 0 _ort_dl_code)
        if(NOT _ort_dl_code EQUAL 0)
            list(GET _ort_dl 1 _ort_dl_msg)
            message(FATAL_ERROR "Could not download ONNX Runtime: ${_ort_dl_msg}\n"
                                "URL: ${_ort_url}")
        endif()

        file(REMOVE_RECURSE "${_ort_root}" "${CMAKE_BINARY_DIR}/ort_extract")
        file(ARCHIVE_EXTRACT INPUT "${_ort_zip}" DESTINATION "${CMAKE_BINARY_DIR}/ort_extract")
        file(MAKE_DIRECTORY "${_ort_root}/bin")
        file(COPY "${CMAKE_BINARY_DIR}/ort_extract/build/native/include/"
             DESTINATION "${_ort_root}/include")
        file(GLOB _ort_dlls "${CMAKE_BINARY_DIR}/ort_extract/runtimes/win-x64/native/*.dll")
        file(COPY ${_ort_dlls} DESTINATION "${_ort_root}/bin")
        # MIT requires the notice to travel with the binaries.
        file(COPY "${CMAKE_BINARY_DIR}/ort_extract/LICENSE"
             DESTINATION "${_ort_root}")
        file(RENAME "${_ort_root}/LICENSE" "${_ort_root}/LICENSE-onnxruntime.txt")
        file(REMOVE_RECURSE "${CMAKE_BINARY_DIR}/ort_extract")
        file(REMOVE "${_ort_zip}")
        file(WRITE "${_ort_stamp}" "${DARTMATIC_ORT_VERSION}\n")
        message(STATUS "ONNX Runtime unpacked into ${_ort_root}")
    else()
        message(STATUS "Using cached ONNX Runtime ${DARTMATIC_ORT_VERSION} in ${_ort_root}")
    endif()

    if(NOT EXISTS "${_ort_root}/include/onnxruntime_c_api.h")
        message(FATAL_ERROR "ONNX Runtime headers missing from ${_ort_root}/include — "
                            "delete ${_ort_root} and re-run cmake to refetch.")
    endif()

    # Optionally fetch the DirectML redistributable. Cached the same way as ORT.
    if(DARTMATIC_FETCH_DIRECTML AND NOT DARTMATIC_DIRECTML_DLL)
        set(_dml_dir "${PROJECT_SOURCE_DIR}/external_libs/directml")
        set(_dml_stamp "${_dml_dir}/.version-${DARTMATIC_DIRECTML_VERSION}")
        if(NOT EXISTS "${_dml_stamp}")
            set(_dml_url "https://api.nuget.org/v3-flatcontainer/microsoft.ai.directml/${DARTMATIC_DIRECTML_VERSION}/microsoft.ai.directml.${DARTMATIC_DIRECTML_VERSION}.nupkg")
            set(_dml_zip "${CMAKE_BINARY_DIR}/directml.nupkg")
            message(STATUS "Fetching DirectML ${DARTMATIC_DIRECTML_VERSION} — ~200 MB, once "
                           "(the package carries every architecture; we keep one DLL)")
            file(DOWNLOAD "${_dml_url}" "${_dml_zip}" STATUS _dml_dl SHOW_PROGRESS)
            list(GET _dml_dl 0 _dml_dl_code)
            if(NOT _dml_dl_code EQUAL 0)
                list(GET _dml_dl 1 _dml_dl_msg)
                message(FATAL_ERROR "Could not download DirectML: ${_dml_dl_msg}\nURL: ${_dml_url}")
            endif()

            file(REMOVE_RECURSE "${_dml_dir}" "${CMAKE_BINARY_DIR}/dml_extract")
            file(ARCHIVE_EXTRACT INPUT "${_dml_zip}" DESTINATION "${CMAKE_BINARY_DIR}/dml_extract")
            file(MAKE_DIRECTORY "${_dml_dir}")
            file(COPY "${CMAKE_BINARY_DIR}/dml_extract/bin/x64-win/DirectML.dll"
                 DESTINATION "${_dml_dir}")
            # DirectML is proprietary; its licence and third-party notices have
            # to travel with the binary.
            file(COPY "${CMAKE_BINARY_DIR}/dml_extract/LICENSE.txt"
                      "${CMAKE_BINARY_DIR}/dml_extract/ThirdPartyNotices.txt"
                 DESTINATION "${_dml_dir}")
            file(RENAME "${_dml_dir}/LICENSE.txt" "${_dml_dir}/LICENSE-directml.txt")
            file(REMOVE_RECURSE "${CMAKE_BINARY_DIR}/dml_extract")
            file(REMOVE "${_dml_zip}")
            file(WRITE "${_dml_stamp}" "${DARTMATIC_DIRECTML_VERSION}\n")
        endif()
        set(DARTMATIC_DIRECTML_DLL "${_dml_dir}/DirectML.dll")
    endif()

    file(GLOB _ort_runtime_dlls "${_ort_root}/bin/*.dll")
    if(DARTMATIC_DIRECTML_DLL)
        list(APPEND _ort_runtime_dlls "${DARTMATIC_DIRECTML_DLL}")
        message(STATUS "Shipping DirectML.dll from ${DARTMATIC_DIRECTML_DLL}")
    else()
        message(STATUS "Using the DirectML.dll that ships with Windows "
                       "(set DARTMATIC_FETCH_DIRECTML=ON if the backend reports a CPU fallback)")
    endif()

    foreach(_t IN LISTS DARTMATIC_TARGETS)
        # SYSTEM: the ONNX Runtime header re-defines the SAL annotation macros
        # (_In_reads_, _Check_return_, ...) that MinGW's windows.h already
        # provides, which -Werror would otherwise turn into a build failure.
        target_include_directories(${_t} SYSTEM PRIVATE "${_ort_root}/include")
        foreach(_dll IN LISTS _ort_runtime_dlls)
            add_custom_command(TARGET ${_t} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_dll}" $<TARGET_FILE_DIR:${_t}>)
        endforeach()
    endforeach()
endif()

if(DARTMATIC_INFER_BACKEND STREQUAL "tensorrt")
    # CUDA runtime — project(LANGUAGES ... CUDA) above already populates
    # CUDAToolkit_*.
    find_package(CUDAToolkit REQUIRED)

    # TensorRT headers + libs. JetPack installs them under the standard
    # /usr/include/aarch64-linux-gnu and /usr/lib/aarch64-linux-gnu paths;
    # we still search explicitly so a missing install produces a clear
    # error pointing at the apt package.
    find_path(TENSORRT_INCLUDE_DIR NvInfer.h
        HINTS /usr/include/aarch64-linux-gnu /usr/include
        DOC "TensorRT include directory")
    find_library(TENSORRT_INFER_LIB nvinfer
        HINTS /usr/lib/aarch64-linux-gnu /usr/lib
        DOC "TensorRT libnvinfer")
    find_library(TENSORRT_ONNXPARSER_LIB nvonnxparser
        HINTS /usr/lib/aarch64-linux-gnu /usr/lib
        DOC "TensorRT libnvonnxparser")
    if(NOT TENSORRT_INCLUDE_DIR OR NOT TENSORRT_INFER_LIB OR NOT TENSORRT_ONNXPARSER_LIB)
        message(FATAL_ERROR
            "TensorRT not found. On JetPack: sudo apt install tensorrt "
            "tensorrt-dev libnvonnxparsers-dev libcudnn9-dev")
    endif()

    foreach(_t IN LISTS DARTMATIC_TARGETS)
        target_include_directories(${_t} PRIVATE ${TENSORRT_INCLUDE_DIR})
        target_link_libraries(${_t} PRIVATE
            CUDA::cudart
            ${TENSORRT_INFER_LIB}
            ${TENSORRT_ONNXPARSER_LIB})
    endforeach()
endif()


# ============================================================================
# Model assets
# ============================================================================
#
# Uses copy_if_different (not copy_directory) on each file individually so the
# source mtime is preserved when content hasn't changed — that's what keeps a
# cached .trt engine next to it valid across rebuilds. The runtime cache check
# invalidates whenever the ONNX is newer than the .trt, and copy_directory
# unconditionally bumps the destination mtime, which would force a multi-minute
# engine rebuild on every cmake --build.

if(_dartmatic_needs_backend)
    set(_dartmatic_onnx_assets
        detect/models/multicam_unet_ar/multicam_unet_ar.onnx
        # Declares which conditioning layout the ONNX beside it expects. Staged
        # like a model because the detector refuses to load without it — an
        # export whose channel layout disagrees with this code has to fail
        # loudly, not quietly score the wrong thing.
        detect/models/multicam_unet_ar/multicam_unet_ar.cond.json
        detect/models/multicam_unet_ar/dart_seg_unet.onnx
        detect/models/palm_detection/blazepalm.onnx
        detect/models/palm_detection/hand_landmark.onnx
    )

    # Staged through the build graph rather than POST_BUILD.
    #
    # POST_BUILD only fires when the target actually relinks, so replacing a
    # model and rebuilding did nothing at all — the executable was up to date,
    # the copy was skipped, and the old model stayed in place. That is exactly
    # how a three-month-stale segmentation model survived a rebuild and cost a
    # long debugging session. A stamp that DEPENDS on the source file makes the
    # copy part of the graph, so touching a model re-stages it.
    #
    # copy_if_different still preserves mtime when the content matches, which is
    # what keeps a cached TensorRT engine valid across rebuilds.
    function(dartmatic_stage_models target)
        set(_stamps "")
        foreach(_asset IN LISTS _dartmatic_onnx_assets)
            get_filename_component(_asset_dir "${_asset}" DIRECTORY)
            string(MAKE_C_IDENTIFIER "${_asset}" _asset_id)
            set(_stamp "${CMAKE_CURRENT_BINARY_DIR}/model_stamps/${target}_${_asset_id}.stamp")
            add_custom_command(
                OUTPUT  "${_stamp}"
                DEPENDS "${PROJECT_SOURCE_DIR}/${_asset}"
                COMMAND ${CMAKE_COMMAND} -E make_directory
                        "$<TARGET_FILE_DIR:${target}>/${_asset_dir}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${PROJECT_SOURCE_DIR}/${_asset}"
                        "$<TARGET_FILE_DIR:${target}>/${_asset}"
                COMMAND ${CMAKE_COMMAND} -E touch "${_stamp}"
                COMMENT "Staging ${_asset}"
                VERBATIM)
            list(APPEND _stamps "${_stamp}")
        endforeach()
        # The executable depends on the models, not the other way round:
        # `cmake --build . --target Dartmatic_Server` builds only that target,
        # so anything hanging off it in the other direction never runs.
        # $<TARGET_FILE_DIR> resolves at generate time, so ordering the staging
        # before the link is fine.
        add_custom_target(${target}_models DEPENDS ${_stamps})
        add_dependencies(${target} ${target}_models)
    endfunction()

    foreach(_t IN LISTS DARTMATIC_TARGETS)
        dartmatic_stage_models(${_t})
    endforeach()
endif()


# ============================================================================
# Portable marker
# ============================================================================
#
# Its presence beside the executable makes app_paths.cpp keep configuration,
# logs and captures next to the binary instead of under the user's profile.
#
# Staged into every build tree and shipped by no installer. That keeps the
# development workflow exactly as it was — everything lands in build/bin where
# you can see it — while an installed copy, which has no business writing inside
# Program Files, uses the user data directory.
foreach(_t IN LISTS DARTMATIC_TARGETS)
    add_custom_command(TARGET ${_t} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E touch "$<TARGET_FILE_DIR:${_t}>/portable.txt"
        COMMENT "Marking ${_t} build tree as a portable install"
        VERBATIM)
endforeach()


# ============================================================================
# External libraries
# ============================================================================

set(EXTERNAL_LIBS ${PROJECT_SOURCE_DIR}/external_libs)
include_directories(${EXTERNAL_LIBS})
add_subdirectory(${EXTERNAL_LIBS})

# Copy the DLLs a target actually loads into its output directory.
#
# $<TARGET_RUNTIME_DLLS> asks the linker what it will look for at run time,
# rather than naming libraries by hand. The hand-written list this replaces used
# $<TARGET_FILE:...>, which for a statically linked dependency is its .a — so
# build/bin accumulated libspdlog.a, libSDL3_ttf.a and friends, none of which do
# anything at run time and all of which would have shipped in the installer.
if(WIN32)
    foreach(_t IN LISTS DARTMATIC_TARGETS)
        # The server links everything statically, so its DLL list is empty and
        # copy_if_different would be invoked with a destination and no sources.
        # Switching the subcommand to `true` is the workaround CMake's own
        # documentation gives for that case; there is no way to skip the command
        # outright, because whether the list is empty is not known until
        # generate time.
        add_custom_command(TARGET ${_t} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E
                    $<IF:$<BOOL:$<TARGET_RUNTIME_DLLS:${_t}>>,copy_if_different,true>
                    "$<TARGET_RUNTIME_DLLS:${_t}>" "$<TARGET_FILE_DIR:${_t}>"
            COMMAND_EXPAND_LISTS)
    endforeach()
endif()


# ============================================================================
# Install rules
# ============================================================================
#
# Every DESTINATION below is relative to the install prefix and several are "."
# itself, which CMake 3.31 started normalising. Opt in where the policy exists
# rather than restructuring around it: the flat layout is deliberate, and
# without this every install rule emits a developer warning that buries
# anything worth reading in the configure output.
if(POLICY CMP0177)
    cmake_policy(SET CMP0177 NEW)
endif()
#
# A flat layout: executable, its DLLs and its assets all in one directory. That
# is what a Windows user expects from an installed program, what a Start Menu
# shortcut assumes, and — not incidentally — what appAssetPath() resolves
# against, so the installed tree and a build tree find their models the same
# way.
#
# Nothing here installs portable.txt. Its absence is what tells an installed
# copy to keep configuration and logs in the user's profile rather than trying
# to write inside Program Files.

# bin/ rather than the install root, because CPack's NSIS generator hardcodes
# that path when it writes the Start Menu shortcut and the "run now" checkbox
# on the finish page. A flat layout produced an installer whose shortcut
# pointed at $INSTDIR\bin\Dartmatic.exe while the executable sat at
# $INSTDIR\Dartmatic.exe — a broken shortcut is a worse first impression than
# one extra directory.
#
# It also makes the installed tree identical in shape to build/bin, so
# "does it behave the same installed as it does here" stops being a question.
set(DARTMATIC_INSTALL_BIN "bin")

if(DARTMATIC_BUILD_APP)
    install(TARGETS ${PRJ_NAME} RUNTIME DESTINATION ${DARTMATIC_INSTALL_BIN}
            COMPONENT dartmatic)
endif()
if(DARTMATIC_BUILD_SERVER)
    install(TARGETS ${SERVER_NAME} RUNTIME DESTINATION ${DARTMATIC_INSTALL_BIN}
            COMPONENT dartmatic)
endif()

# ---- Assets -----------------------------------------------------------------
#
# The Kenney input-prompt pack is 2648 icons and 15 MB; this program loads 35 of
# them. Listing those explicitly keeps the installer small enough to send to
# someone. A name that drifts out of this list shows up immediately as a
# "Failed to load image" line and a blank input hint, which is why the list can
# be maintained by hand without becoming a trap.
if(DARTMATIC_BUILD_APP)
    # The window icon is loaded at runtime, so it has to be in the installed
    # asset tree as well as the platform icon locations.
    install(FILES "${PROJECT_SOURCE_DIR}/assets/branding/icon-256.png"
            DESTINATION "${DARTMATIC_INSTALL_BIN}/assets/branding"
            COMPONENT dartmatic)

    install(FILES "${PROJECT_SOURCE_DIR}/assets/fonts/Roboto-Regular.ttf"
            DESTINATION "${DARTMATIC_INSTALL_BIN}/assets/fonts"
            COMPONENT dartmatic)

    set(_kenney_root "assets/images/kenney_input-prompts_1.4.1")
    set(_kenney_keyboard
        keyboard_a.png keyboard_c.png keyboard_d.png keyboard_e.png
        keyboard_f.png keyboard_q.png keyboard_r.png keyboard_s.png
        keyboard_w.png
        keyboard_arrow_down.png keyboard_arrow_left.png
        keyboard_arrow_right.png keyboard_arrow_up.png
        keyboard_backspace.png keyboard_bracket_close.png
        keyboard_bracket_open.png keyboard_delete.png keyboard_enter.png
        keyboard_escape.png keyboard_home.png keyboard_space.png
        keyboard_tab.png)
    set(_kenney_xbox
        xbox_button_back.png xbox_button_start.png
        xbox_button_color_a.png xbox_button_color_b.png
        xbox_button_color_x.png xbox_button_color_y.png
        xbox_dpad_down.png xbox_dpad_left.png xbox_dpad_right.png
        xbox_dpad_up.png xbox_lb.png xbox_rb.png xbox_stick_l.png)

    foreach(_icon IN LISTS _kenney_keyboard)
        install(FILES "${PROJECT_SOURCE_DIR}/${_kenney_root}/Keyboard & Mouse/Default/${_icon}"
                DESTINATION "${DARTMATIC_INSTALL_BIN}/${_kenney_root}/Keyboard & Mouse/Default"
            COMPONENT dartmatic)
    endforeach()
    foreach(_icon IN LISTS _kenney_xbox)
        install(FILES "${PROJECT_SOURCE_DIR}/${_kenney_root}/Xbox Series/Default/${_icon}"
                DESTINATION "${DARTMATIC_INSTALL_BIN}/${_kenney_root}/Xbox Series/Default"
            COMPONENT dartmatic)
    endforeach()
endif()

# ---- Models -----------------------------------------------------------------
#
# Only configurations that actually run inference carry the 44 MB of ONNX. The
# Pi client ships none of it: its whole purpose is that the models live on the
# other end of the socket.
if(_dartmatic_needs_backend)
    foreach(_asset IN LISTS _dartmatic_onnx_assets)
        get_filename_component(_asset_dir "${_asset}" DIRECTORY)
        install(FILES "${PROJECT_SOURCE_DIR}/${_asset}"
                DESTINATION "${DARTMATIC_INSTALL_BIN}/${_asset_dir}"
            COMPONENT dartmatic)
    endforeach()
endif()

# ---- Runtime libraries ------------------------------------------------------
#
# Shipping onnxruntime.dll is not optional and not merely tidy: Windows carries
# its own onnxruntime.dll in System32 (1.17.1 as of this writing), and if ours
# is not sitting beside the executable the loader finds that one instead. It is
# too old for the API version this build pins, so the backend refuses to start.
# The failure is at least loud — the version check catches it — but the fix is
# to make sure the loader never has to look further than our own directory.
if(WIN32)
    # Note: no install(FILES $<TARGET_RUNTIME_DLLS:...>) here. That genex missed
    # SDL3_image.dll and SDL3_ttf.dll even though the executable imports both,
    # and mixing it with the dependency scan below meant two mechanisms could
    # each believe the other had covered a DLL. The scan at the end of this
    # section resolves every runtime library from the binaries themselves, which
    # cannot disagree with what the loader will do.

    if(DARTMATIC_INFER_BACKEND STREQUAL "directml")
        install(FILES ${_ort_runtime_dlls}
                DESTINATION ${DARTMATIC_INSTALL_BIN}
                COMPONENT dartmatic)

        # Only present when this tree fetched ONNX Runtime itself; a cache from
        # before the licence started being kept will not have it. Warn rather
        # than fail the configure, but do say so — shipping the binary without
        # its notice is not something to discover after publishing.
        set(_ort_license "${PROJECT_SOURCE_DIR}/external_libs/onnxruntime/LICENSE-onnxruntime.txt")
        if(EXISTS "${_ort_license}")
            install(FILES "${_ort_license}"
                    DESTINATION "${DARTMATIC_INSTALL_BIN}/licenses"
                    COMPONENT dartmatic)
        else()
            message(WARNING
                "ONNX Runtime's licence is missing from external_libs/onnxruntime. "
                "Packages built from this tree would ship the DLL without its notice. "
                "Delete external_libs/onnxruntime and re-run cmake to refetch it.")
        endif()

        if(DARTMATIC_DIRECTML_DLL)
            # Proprietary: the licence and third-party notices travel with it.
            install(FILES
                "${PROJECT_SOURCE_DIR}/external_libs/directml/LICENSE-directml.txt"
                "${PROJECT_SOURCE_DIR}/external_libs/directml/ThirdPartyNotices.txt"
                DESTINATION "${DARTMATIC_INSTALL_BIN}/licenses"
                COMPONENT dartmatic)
        endif()
    endif()
endif()

# ---- Linux desktop integration ---------------------------------------------
#
# Installed inside the package prefix; the postinst symlinks it into
# /usr/share/applications. Putting it there directly would mean an absolute
# DESTINATION escaping CPACK_PACKAGING_INSTALL_PREFIX, which the archive
# generators handle differently from DEB.
if(UNIX AND NOT APPLE AND DARTMATIC_BUILD_APP)
    install(FILES "${PROJECT_SOURCE_DIR}/packaging/linux/dartmatic.desktop"
            DESTINATION "share/applications"
            COMPONENT dartmatic)
    # Named for the product at install time, because the .desktop Icon= key and
    # the installed filename have to agree. The source file stays name-neutral
    # so a rename never has to touch the branding directory.
    string(TOLOWER "${PRJ_NAME}" _dm_icon_name)
    install(FILES "${PROJECT_SOURCE_DIR}/assets/branding/icon-256.png"
            DESTINATION "share/icons/hicolor/256x256/apps"
            RENAME "${_dm_icon_name}.png"
            COMPONENT dartmatic)
endif()

install(FILES "${PROJECT_SOURCE_DIR}/LICENSE"
        DESTINATION "${DARTMATIC_INSTALL_BIN}/licenses"
        RENAME "LICENSE-dartmatic.txt"
        COMPONENT dartmatic)

# ---- Toolchain DLLs the shipped libraries need ------------------------------
#
# Runs last, because it inspects what the earlier rules have already put in the
# install tree. See cmake/install_runtime_deps.cmake.in for why this is
# resolved rather than listed.
if(WIN32 AND MINGW)
    set(DARTMATIC_INSTALLED_EXECUTABLES "")
    if(DARTMATIC_BUILD_APP)
        list(APPEND DARTMATIC_INSTALLED_EXECUTABLES "${PRJ_NAME}.exe")
    endif()
    if(DARTMATIC_BUILD_SERVER)
        list(APPEND DARTMATIC_INSTALLED_EXECUTABLES "${SERVER_NAME}.exe")
    endif()

    get_filename_component(DARTMATIC_TOOLCHAIN_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)

    # The build output directory has to come from the target itself.
    # CMAKE_RUNTIME_OUTPUT_DIRECTORY is not set by this project — the bin/
    # subdirectory arrives from elsewhere in the configure — so reading it here
    # silently yielded an empty path and the scan found nothing at all.
    #
    # $<TARGET_FILE_DIR> is only available to install(CODE), not install(SCRIPT),
    # so the value is handed over through a variable the script then reads: both
    # run in the same scope during installation.
    list(GET DARTMATIC_TARGETS 0 _dm_first_target)
    install(CODE
        "set(DARTMATIC_BIN_DIR [[$<TARGET_FILE_DIR:${_dm_first_target}>]])"
        COMPONENT dartmatic)

    configure_file(
        "${PROJECT_SOURCE_DIR}/cmake/install_runtime_deps.cmake.in"
        "${PROJECT_BINARY_DIR}/install_runtime_deps.cmake"
        @ONLY)
    install(SCRIPT "${PROJECT_BINARY_DIR}/install_runtime_deps.cmake"
            COMPONENT dartmatic)
endif()


# ============================================================================
# Packaging
# ============================================================================
#
# CPack turns the install rules above into a distributable artifact. Nothing
# here is needed by whoever receives it: NSIS runs on this machine and emits a
# self-contained Setup.exe.
#
# Each configuration produces its own artifact rather than one installer with
# selectable components, because they are genuinely separate builds — the game
# and the server disagree about SDL, about OpenCV's module list, and about
# whether there is an inference backend at all. Merging them would need a
# superbuild, which is more machinery than three audiences justify.

set(CPACK_PACKAGE_NAME              "Dartmatic")
set(CPACK_PACKAGE_VENDOR            "ippm12")
set(CPACK_PACKAGE_VERSION           "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Automatic dart scoring and party games for a real dartboard")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Dartmatic")
set(CPACK_RESOURCE_FILE_LICENSE     "${PROJECT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_CHECKSUM          SHA256)

# Package only our own component.
#
# OpenCV and the SDL family bring their own install rules — headers, static
# libraries, CMake config, haarcascade XML — and a package built without this
# filter came to 155 MB of which about 45 MB was anything a user needs. Those
# rules land in the "Unspecified" component; every rule of ours above names
# "dartmatic", so listing it here is what separates the two. The install stays
# monolithic: there is no component-selection page, just a smaller payload.
set(CPACK_COMPONENTS_ALL dartmatic)
set(CPACK_COMPONENTS_GROUPING ALL_COMPONENTS_IN_ONE)

# CPACK_COMPONENTS_ALL is only consulted when the generator is doing a
# component install; left off, each generator quietly falls back to packaging
# the whole install tree and the filter above does nothing. Grouping everything
# into one component keeps the result a single archive and, for NSIS, an
# installer with no component-selection page — the filtering is the only part
# of the component machinery being used here.
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
set(CPACK_NSIS_COMPONENT_INSTALL    ON)
set(CPACK_DEB_COMPONENT_INSTALL     ON)

# Component installs default to omitting the top-level folder. Whether that is
# right depends entirely on the generator:
#
#   zip       needs one, or unpacking sprays 39 loose files into whatever
#             directory the user happened to be in;
#   installer must not have one, because the directory the user chose is
#             already the container — otherwise everything lands in
#             C:\Program Files\Dartmatic\Dartmatic-0.1.0-AMD64\ and the Start
#             Menu shortcut points at a path that does not exist.
#
# Set below, next to the generator choice, rather than here where it would have
# to guess.

# What this particular build is, so the four artifacts do not collide in a
# releases directory and nobody has to guess which one they downloaded.
if(DARTMATIC_BUILD_SERVER)
    set(_pkg_variant "Server")
    set(CPACK_PACKAGE_EXECUTABLES "")
elseif(DARTMATIC_VISION_SOURCE STREQUAL "sim")
    set(_pkg_variant "Demo")
elseif(DARTMATIC_VISION_SOURCE STREQUAL "network")
    set(_pkg_variant "Client")
else()
    set(_pkg_variant "")
endif()

if(_pkg_variant)
    set(CPACK_PACKAGE_FILE_NAME "Dartmatic-${_pkg_variant}-${PROJECT_VERSION}-${CMAKE_SYSTEM_PROCESSOR}")
else()
    set(CPACK_PACKAGE_FILE_NAME "Dartmatic-${PROJECT_VERSION}-${CMAKE_SYSTEM_PROCESSOR}")
endif()

if(WIN32)
    # The sim build is a demo: a folder you unzip and run, with no installer,
    # no shortcuts and nothing to uninstall. Anything that touches real
    # hardware gets a proper installed presence.
    if(DARTMATIC_VISION_SOURCE STREQUAL "sim" AND NOT DARTMATIC_BUILD_SERVER)
        set(CPACK_GENERATOR "ZIP")
        set(CPACK_COMPONENT_INCLUDE_TOPLEVEL_DIRECTORY 1)
    else()
        set(CPACK_GENERATOR "NSIS")
        set(CPACK_COMPONENT_INCLUDE_TOPLEVEL_DIRECTORY 0)
    endif()

    set(CPACK_NSIS_MUI_ICON   "${PROJECT_SOURCE_DIR}/assets/branding/icon.ico")
    set(CPACK_NSIS_MUI_UNIICON "${PROJECT_SOURCE_DIR}/assets/branding/icon.ico")
    set(CPACK_NSIS_INSTALLED_ICON_NAME
        "${DARTMATIC_INSTALL_BIN}\\\\${PRJ_NAME}.exe")
    set(CPACK_NSIS_PACKAGE_NAME     "Dartmatic ${_pkg_variant}")
    set(CPACK_NSIS_DISPLAY_NAME     "Dartmatic ${_pkg_variant} ${PROJECT_VERSION}")
    set(CPACK_NSIS_MODIFY_PATH      OFF)
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)

    if(DARTMATIC_BUILD_APP)
        set(CPACK_PACKAGE_EXECUTABLES "${PRJ_NAME}" "Dartmatic")
        set(CPACK_NSIS_MUI_FINISHPAGE_RUN "${PRJ_NAME}.exe")
    endif()

    # The server listens on a TCP port, so Windows will put a firewall prompt in
    # front of the first client that tries to connect — from another machine,
    # where nobody is watching this one to click Allow. Adding the rule at
    # install time is the difference between "it works" and a silent timeout.
    #
    # Escaping here is doubled because the text is parsed twice: once as this
    # string, and again when CPack writes it verbatim into CPackConfig.cmake and
    # re-reads that file. CPack does no escaping of its own, so whatever this
    # evaluates to has to already be valid inside a CMake quoted string.
    # A path written \\ therefore arrives as \, which CPackConfig.cmake then
    # rejects as the invalid escape \D — hence four backslashes, not two.
    if(DARTMATIC_BUILD_SERVER)
        set(CPACK_NSIS_EXTRA_INSTALL_COMMANDS "
            ExecWait 'netsh advfirewall firewall add rule name=\\\"Dartmatic Server\\\" dir=in action=allow program=\\\"$INSTDIR\\\\${DARTMATIC_INSTALL_BIN}\\\\${SERVER_NAME}.exe\\\" enable=yes'
        ")
        set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS "
            ExecWait 'netsh advfirewall firewall delete rule name=\\\"Dartmatic Server\\\"'
        ")
    endif()
else()
    set(CPACK_GENERATOR "TGZ;DEB")
    # The .deb unpacks straight into /opt/dartmatic, so a versioned wrapper
    # directory inside it would just be in the way. The .tar.gz gets one from
    # its own layout rules.
    set(CPACK_COMPONENT_INCLUDE_TOPLEVEL_DIRECTORY 0)
    set(CPACK_PACKAGING_INSTALL_PREFIX "/opt/dartmatic")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "ippm12")
    set(CPACK_DEBIAN_PACKAGE_SECTION    "games")
    set(CPACK_DEBIAN_FILE_NAME          DEB-DEFAULT)

    # Debian package names are lowercase, and there is only ever one component
    # here -- leaving component install on would emit "Dartmatic-dartmatic_..."
    # instead of the "dartmatic_0.1.0_arm64.deb" a user expects to download.
    set(CPACK_DEB_COMPONENT_INSTALL     OFF)
    set(CPACK_DEBIAN_PACKAGE_NAME       "dartmatic")
    set(CPACK_DEBIAN_PACKAGE_HOMEPAGE   "https://github.com/ippm12/Dartmatic")

    # /opt is outside PATH and outside the desktop menu, so the package links
    # itself into both on install and unlinks on removal.
    set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
        "${PROJECT_SOURCE_DIR}/packaging/linux/postinst;${PROJECT_SOURCE_DIR}/packaging/linux/prerm")
    # Left to dpkg-shlibdeps: the Pi client links system OpenCV and SDL
    # transitively and hand-listing them goes stale the first time a dependency
    # moves.
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
endif()

include(CPack)
