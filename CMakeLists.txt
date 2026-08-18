cmake_minimum_required(VERSION 3.22.1)

# CMAKE_CXX_STANDARD set as a directory variable would leak into OpenCV /
# SDL subprojects and interact badly with their own compiler-feature
# checks. We apply cxx_std_23 to our targets explicitly via
# target_compile_features() below instead.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(PRJ_NAME Treblotron)
set(SERVER_NAME Treblotron_Server)

# ============================================================================
# Configuration
# ============================================================================
#
# Two independent axes:
#
#   TREBLOTRON_VISION_SOURCE  where the game gets dart events from
#   TREBLOTRON_INFER_BACKEND  what actually executes a forward pass
#
# They're separate because the same inference backend serves two very
# different binaries: the game running detection locally, and the headless
# server running it on behalf of a remote client. Typical combinations:
#
#   dev workstation      VISION_SOURCE=sim      INFER_BACKEND=none
#   Jetson, standalone   VISION_SOURCE=local    INFER_BACKEND=tensorrt
#   Raspberry Pi + server VISION_SOURCE=network INFER_BACKEND=none
#   inference server     BUILD_APP=OFF BUILD_SERVER=ON INFER_BACKEND=cpu|tensorrt

option(TREBLOTRON_BUILD_APP    "Build the Treblotron game application"    ON)
option(TREBLOTRON_BUILD_SERVER "Build the headless inference server"     OFF)
option(TREBLOTRON_SHOW_FPS     "Show framerate counter in the top right" ON)

set(TREBLOTRON_VISION_SOURCE "sim"  CACHE STRING "Vision source: sim|local|network")
set(TREBLOTRON_INFER_BACKEND "none" CACHE STRING "Inference backend: none|tensorrt|directml|cpu")
set_property(CACHE TREBLOTRON_VISION_SOURCE PROPERTY STRINGS sim local network)
set_property(CACHE TREBLOTRON_INFER_BACKEND PROPERTY STRINGS none tensorrt directml cpu)

# Pinned so a silent upstream bump can't change inference behaviour underfoot.
set(TREBLOTRON_ORT_VERSION "1.24.4" CACHE STRING "ONNX Runtime version for the directml backend")
set(TREBLOTRON_DIRECTML_VERSION "1.15.4" CACHE STRING "DirectML redistributable version")

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
option(TREBLOTRON_FETCH_DIRECTML "Ship the DirectML redistributable instead of using the Windows copy" OFF)

# Back-compat with the previous boolean flags so existing command lines,
# .vscode configs and build caches keep working.
#
# Two rules keep a legacy flag from outliving its usefulness:
#
#   - it only applies when the new variable is still at its default, so an
#     explicit -DTREBLOTRON_VISION_SOURCE always wins;
#   - it is removed from the cache once honoured, so it cannot silently steer
#     every future configure of the same build directory.
#
# Both matter because the old flags live in existing build caches. Without
# them, reconfiguring a build directory that once had TREBLOTRON_USE_HAILO=ON
# would keep demanding HailoRT no matter what you asked for — which is exactly
# what happens when a Pi's accelerator is removed and the build tree isn't.
# That flag is now gone entirely, but it is still cleared below, because the
# stale caches that made it a problem are still out there.
macro(_treblotron_retire_flag legacy explanation)
    if(${legacy})
        if(_treblotron_source_is_default)
            message(DEPRECATION "${legacy} is deprecated — ${explanation}")
            set(_treblotron_legacy_applied TRUE)
        else()
            message(WARNING
                "Ignoring stale ${legacy} from the build cache: "
                "TREBLOTRON_VISION_SOURCE=${TREBLOTRON_VISION_SOURCE} was requested explicitly. "
                "Clearing ${legacy} so it stops interfering.")
        endif()
    endif()
    unset(${legacy} CACHE)
endmacro()

set(_treblotron_source_is_default FALSE)
if(TREBLOTRON_VISION_SOURCE STREQUAL "sim")
    set(_treblotron_source_is_default TRUE)
endif()

# Hailo support was removed in v0.1.0: the accelerator path targeted a model
# generation two contracts old, ran its own detection pipeline rather than the
# shared one, and loaded a .hef that was never in this repository. Say so
# outright rather than silently ignoring the flag — someone reconfiguring an old
# build tree deserves to know the option went away rather than wonder why their
# setting had no effect.
if(TREBLOTRON_USE_HAILO)
    message(WARNING
        "TREBLOTRON_USE_HAILO is set, but Hailo support was removed in v0.1.0. "
        "Clearing it. Use -DTREBLOTRON_VISION_SOURCE=network for a Pi that streams "
        "to an inference server, or =local to run the models on this machine.")
endif()
unset(TREBLOTRON_USE_HAILO CACHE)

set(_treblotron_legacy_applied FALSE)
_treblotron_retire_flag(TREBLOTRON_USE_TENSORRT
    "use -DTREBLOTRON_VISION_SOURCE=local -DTREBLOTRON_INFER_BACKEND=tensorrt")
if(_treblotron_legacy_applied)
    set(TREBLOTRON_VISION_SOURCE "local"    CACHE STRING "Vision source"      FORCE)
    set(TREBLOTRON_INFER_BACKEND "tensorrt" CACHE STRING "Inference backend"  FORCE)
endif()

# Never read; retired so it cannot confuse anyone reading a stale cache.
unset(TREBLOTRON_USE_SIM CACHE)

if(NOT TREBLOTRON_VISION_SOURCE MATCHES "^(sim|local|network)$")
    message(FATAL_ERROR "TREBLOTRON_VISION_SOURCE must be sim, local or network "
                        "(got '${TREBLOTRON_VISION_SOURCE}')")
endif()
if(NOT TREBLOTRON_INFER_BACKEND MATCHES "^(none|tensorrt|directml|cpu)$")
    message(FATAL_ERROR "TREBLOTRON_INFER_BACKEND must be none, tensorrt, directml or cpu "
                        "(got '${TREBLOTRON_INFER_BACKEND}')")
endif()
if(TREBLOTRON_INFER_BACKEND STREQUAL "directml" AND NOT WIN32)
    message(FATAL_ERROR "The directml backend is Windows-only (DirectML is a DirectX 12 "
                        "component). Use tensorrt on Jetson, or cpu elsewhere.")
endif()
if(NOT TREBLOTRON_BUILD_APP AND NOT TREBLOTRON_BUILD_SERVER)
    message(FATAL_ERROR "Nothing to build — enable TREBLOTRON_BUILD_APP and/or TREBLOTRON_BUILD_SERVER")
endif()

# The pieces that need a real backend: the local vision source, and the server.
set(_treblotron_needs_backend OFF)
if(TREBLOTRON_BUILD_APP AND TREBLOTRON_VISION_SOURCE STREQUAL "local")
    set(_treblotron_needs_backend ON)
endif()
if(TREBLOTRON_BUILD_SERVER)
    set(_treblotron_needs_backend ON)
endif()
if(_treblotron_needs_backend AND TREBLOTRON_INFER_BACKEND STREQUAL "none")
    message(FATAL_ERROR
        "This configuration runs the detection models, so it needs an inference "
        "backend. Add -DTREBLOTRON_INFER_BACKEND=tensorrt (Jetson) or =cpu (OpenCV DNN).")
endif()

# CUDA has to be declared in project(LANGUAGES ...) — more robust on
# CMake 3.22 / JetPack than enable_language(CUDA) after project().
set(_TREBLOTRON_LANGUAGES C CXX)
if(TREBLOTRON_INFER_BACKEND STREQUAL "tensorrt")
    if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
        # Orin Nano Super: Ampere, compute capability 8.7.
        set(CMAKE_CUDA_ARCHITECTURES 87)
    endif()
    list(APPEND _TREBLOTRON_LANGUAGES CUDA)
endif()

project(${PRJ_NAME}
        VERSION   0.1.0
        LANGUAGES ${_TREBLOTRON_LANGUAGES})

# Surfaced in the startup log, on the main menu and via --version, so a bug
# report can name the build it came from. Generated rather than hand-written in
# a header, so the number lives in exactly one place.
configure_file(
    "${PROJECT_SOURCE_DIR}/inc_common/treblotron_version.hpp.in"
    "${PROJECT_BINARY_DIR}/generated/treblotron_version.hpp"
    @ONLY)

message(STATUS "Treblotron ${PROJECT_VERSION}: app=${TREBLOTRON_BUILD_APP} "
               "server=${TREBLOTRON_BUILD_SERVER} "
               "vision_source=${TREBLOTRON_VISION_SOURCE} backend=${TREBLOTRON_INFER_BACKEND}")


# ============================================================================
# Module helper
# ============================================================================
#
# A module is a directory with public headers in <mod>/inc_public/<mod>/ and
# flat implementation files in <mod>/*.cpp. Globbed with CONFIGURE_DEPENDS so
# there are no source lists to maintain. Two targets share this, which is why
# it's a function rather than the inline loop it used to be.

function(treblotron_add_module target module)
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

# Shared settings every Treblotron target wants.
function(treblotron_configure_target target)
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

    if(TREBLOTRON_INFER_BACKEND STREQUAL "cpu")
        target_compile_definitions(${target} PRIVATE TREBLOTRON_INFER_CPU)
    elseif(TREBLOTRON_INFER_BACKEND STREQUAL "tensorrt")
        target_compile_definitions(${target} PRIVATE TREBLOTRON_INFER_TENSORRT)
    elseif(TREBLOTRON_INFER_BACKEND STREQUAL "directml")
        target_compile_definitions(${target} PRIVATE TREBLOTRON_INFER_DIRECTML)
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
    ${PROJECT_SOURCE_DIR}/vision/${TREBLOTRON_VISION_SOURCE}_vision_source.cpp)
foreach(_src IN LISTS _all_vision_sources)
    if(NOT _src STREQUAL _active_vision_source OR NOT TREBLOTRON_BUILD_APP)
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
    if(NOT _src STREQUAL "${_backend_source_${TREBLOTRON_INFER_BACKEND}}")
        set_source_files_properties(${_src} PROPERTIES HEADER_FILE_ONLY TRUE)
    endif()
endforeach()
if(TREBLOTRON_INFER_BACKEND STREQUAL "none")
    set_source_files_properties(${PROJECT_SOURCE_DIR}/detect/dart_detector.cpp
        PROPERTIES HEADER_FILE_ONLY TRUE)
endif()


# ============================================================================
# Game application
# ============================================================================

if(TREBLOTRON_BUILD_APP)
    # ---- Windows executable icon --------------------------------------
    #
    # Without an embedded icon the shipped exe shows the generic MinGW
    # placeholder in Explorer, the taskbar and Alt-Tab -- the installer icon
    # does not cover any of those. windres compiles the .rc; the path is
    # configured in rather than hard-coded so the build tree stays relocatable.
    set(_dm_app_sources startup.cpp)
    if(WIN32)
        enable_language(RC)
        set(TREBLOTRON_ICON_PATH "${PROJECT_SOURCE_DIR}/assets/branding/icon.ico")
        # windres reads the path out of a C string, so backslashes would be
        # escapes. Forward slashes work and avoid the whole question.
        string(REPLACE "\\" "/" TREBLOTRON_ICON_PATH "${TREBLOTRON_ICON_PATH}")
        configure_file("${PROJECT_SOURCE_DIR}/packaging/windows/app.rc.in"
                       "${PROJECT_BINARY_DIR}/app.rc" @ONLY)
        list(APPEND _dm_app_sources "${PROJECT_BINARY_DIR}/app.rc")
    endif()

    add_executable(${PRJ_NAME} ${_dm_app_sources})
    treblotron_configure_target(${PRJ_NAME})

    set(APP_MODULES_LIST "debug" "frame" "dart" "players" "game_lib" "games"
                         "detect" "net" "vision")
    foreach(lib IN LISTS APP_MODULES_LIST)
        message("Including ${lib} library")
        treblotron_add_module(${PRJ_NAME} ${lib})
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

    string(TOUPPER ${TREBLOTRON_VISION_SOURCE} _vision_source_upper)
    target_compile_definitions(${PRJ_NAME} PRIVATE TREBLOTRON_USE_${_vision_source_upper})

    if(TREBLOTRON_VISION_SOURCE STREQUAL "local")
        # The capture threads pre-compute seg input and the detector publishes
        # its own warped frames — see camera_api.cpp.
        target_compile_definitions(${PRJ_NAME} PRIVATE TREBLOTRON_HAVE_LOCAL_INFERENCE)
    endif()

    if(TREBLOTRON_VISION_SOURCE STREQUAL "network")
        # Forward the cameras' own MJPEG instead of decoding and re-encoding it.
        target_compile_definitions(${PRJ_NAME} PRIVATE TREBLOTRON_PASSTHROUGH_CAPTURE)
    endif()

    if(TREBLOTRON_SHOW_FPS)
        target_compile_definitions(${PRJ_NAME} PRIVATE TREBLOTRON_SHOW_FPS)
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

if(TREBLOTRON_BUILD_SERVER)
    add_executable(${SERVER_NAME} server/server_main.cpp)
    treblotron_configure_target(${SERVER_NAME})

    set(SERVER_MODULES_LIST "debug" "dart" "detect" "net" "server")
    foreach(lib IN LISTS SERVER_MODULES_LIST)
        message("Including ${lib} library (server)")
        treblotron_add_module(${SERVER_NAME} ${lib})
    endforeach()
endif()


# ============================================================================
# Backend dependencies
# ============================================================================

set(TREBLOTRON_TARGETS "")
if(TREBLOTRON_BUILD_APP)
    list(APPEND TREBLOTRON_TARGETS ${PRJ_NAME})
endif()
if(TREBLOTRON_BUILD_SERVER)
    list(APPEND TREBLOTRON_TARGETS ${SERVER_NAME})
endif()

# Winsock. Both targets link the net module — the server always, the app
# whenever the network vision source is in play — and ws2_32 is harmless
# otherwise.
if(WIN32)
    foreach(_t IN LISTS TREBLOTRON_TARGETS)
        target_link_libraries(${_t} PRIVATE ws2_32)
    endforeach()
endif()

if(TREBLOTRON_INFER_BACKEND STREQUAL "directml")
    # ONNX Runtime ships as MSVC-built binaries, but that is not a problem
    # here: ort_backend.cpp reaches the library through GetProcAddress on the
    # single exported C function OrtGetApiBase, so there is no link-time
    # dependency and no C++ ABI to match. We only need the headers at build
    # time and the DLLs next to the executable at run time.
    #
    # DirectML.dll itself is NOT fetched — Windows ships one in System32 (1.15.5
    # on this machine, newer than the redistributable), and the loader finds it
    # automatically. Point TREBLOTRON_DIRECTML_DLL at a copy from the
    # Microsoft.AI.DirectML package if a target machine's inbox version is too
    # old for the pinned ONNX Runtime.
    set(_ort_root "${PROJECT_SOURCE_DIR}/external_libs/onnxruntime")
    set(_ort_stamp "${_ort_root}/.version-${TREBLOTRON_ORT_VERSION}")

    if(NOT EXISTS "${_ort_stamp}")
        set(_ort_url "https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime.directml/${TREBLOTRON_ORT_VERSION}/microsoft.ml.onnxruntime.directml.${TREBLOTRON_ORT_VERSION}.nupkg")
        set(_ort_zip "${CMAKE_BINARY_DIR}/ort.nupkg")
        message(STATUS "Fetching ONNX Runtime ${TREBLOTRON_ORT_VERSION} (DirectML) — ~12 MB, once")
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
        file(WRITE "${_ort_stamp}" "${TREBLOTRON_ORT_VERSION}\n")
        message(STATUS "ONNX Runtime unpacked into ${_ort_root}")
    else()
        message(STATUS "Using cached ONNX Runtime ${TREBLOTRON_ORT_VERSION} in ${_ort_root}")
    endif()

    if(NOT EXISTS "${_ort_root}/include/onnxruntime_c_api.h")
        message(FATAL_ERROR "ONNX Runtime headers missing from ${_ort_root}/include — "
                            "delete ${_ort_root} and re-run cmake to refetch.")
    endif()

    # Optionally fetch the DirectML redistributable. Cached the same way as ORT.
    if(TREBLOTRON_FETCH_DIRECTML AND NOT TREBLOTRON_DIRECTML_DLL)
        set(_dml_dir "${PROJECT_SOURCE_DIR}/external_libs/directml")
        set(_dml_stamp "${_dml_dir}/.version-${TREBLOTRON_DIRECTML_VERSION}")
        if(NOT EXISTS "${_dml_stamp}")
            set(_dml_url "https://api.nuget.org/v3-flatcontainer/microsoft.ai.directml/${TREBLOTRON_DIRECTML_VERSION}/microsoft.ai.directml.${TREBLOTRON_DIRECTML_VERSION}.nupkg")
            set(_dml_zip "${CMAKE_BINARY_DIR}/directml.nupkg")
            message(STATUS "Fetching DirectML ${TREBLOTRON_DIRECTML_VERSION} — ~200 MB, once "
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
            file(WRITE "${_dml_stamp}" "${TREBLOTRON_DIRECTML_VERSION}\n")
        endif()
        set(TREBLOTRON_DIRECTML_DLL "${_dml_dir}/DirectML.dll")
    endif()

    file(GLOB _ort_runtime_dlls "${_ort_root}/bin/*.dll")
    if(TREBLOTRON_DIRECTML_DLL)
        list(APPEND _ort_runtime_dlls "${TREBLOTRON_DIRECTML_DLL}")
        message(STATUS "Shipping DirectML.dll from ${TREBLOTRON_DIRECTML_DLL}")
    else()
        message(STATUS "Using the DirectML.dll that ships with Windows "
                       "(set TREBLOTRON_FETCH_DIRECTML=ON if the backend reports a CPU fallback)")
    endif()

    foreach(_t IN LISTS TREBLOTRON_TARGETS)
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

if(TREBLOTRON_INFER_BACKEND STREQUAL "tensorrt")
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

    foreach(_t IN LISTS TREBLOTRON_TARGETS)
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

if(_treblotron_needs_backend)
    set(_treblotron_onnx_assets
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
    function(treblotron_stage_models target)
        set(_stamps "")
        foreach(_asset IN LISTS _treblotron_onnx_assets)
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
        # `cmake --build . --target Treblotron_Server` builds only that target,
        # so anything hanging off it in the other direction never runs.
        # $<TARGET_FILE_DIR> resolves at generate time, so ordering the staging
        # before the link is fine.
        add_custom_target(${target}_models DEPENDS ${_stamps})
        add_dependencies(${target} ${target}_models)
    endfunction()

    foreach(_t IN LISTS TREBLOTRON_TARGETS)
        treblotron_stage_models(${_t})
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
foreach(_t IN LISTS TREBLOTRON_TARGETS)
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
    foreach(_t IN LISTS TREBLOTRON_TARGETS)
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

        # ...then the DLLs those DLLs need. $<TARGET_RUNTIME_DLLS> does not
        # recurse, so it delivers SDL3_ttf.dll and stops -- leaving its own
        # import of libwinpthread-1.dll to be satisfied by MSYS2 being on PATH,
        # which is true here and nowhere else. Runs after the copy above,
        # because it inspects what that copy produced.
        if(MINGW)
            get_filename_component(_dm_toolchain_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
            add_custom_command(TARGET ${_t} POST_BUILD
                COMMAND ${CMAKE_COMMAND}
                        -DBIN_DIR=$<TARGET_FILE_DIR:${_t}>
                        -DEXECUTABLE=$<TARGET_FILE:${_t}>
                        -DTOOLCHAIN_BIN=${_dm_toolchain_bin}
                        -P "${PROJECT_SOURCE_DIR}/cmake/bundle_runtime_deps.cmake"
                COMMENT "Bundling toolchain DLLs for ${_t}"
                VERBATIM)
        endif()
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
# pointed at $INSTDIR\bin\Treblotron.exe while the executable sat at
# $INSTDIR\Treblotron.exe — a broken shortcut is a worse first impression than
# one extra directory.
#
# It also makes the installed tree identical in shape to build/bin, so
# "does it behave the same installed as it does here" stops being a question.
set(TREBLOTRON_INSTALL_BIN "bin")

if(TREBLOTRON_BUILD_APP)
    install(TARGETS ${PRJ_NAME} RUNTIME DESTINATION ${TREBLOTRON_INSTALL_BIN}
            COMPONENT treblotron)
endif()
if(TREBLOTRON_BUILD_SERVER)
    install(TARGETS ${SERVER_NAME} RUNTIME DESTINATION ${TREBLOTRON_INSTALL_BIN}
            COMPONENT treblotron)
endif()

# ---- Assets -----------------------------------------------------------------
#
# The Kenney input-prompt pack is 2648 icons and 15 MB; this program loads 35 of
# them. Listing those explicitly keeps the installer small enough to send to
# someone. A name that drifts out of this list shows up immediately as a
# "Failed to load image" line and a blank input hint, which is why the list can
# be maintained by hand without becoming a trap.
if(TREBLOTRON_BUILD_APP)
    # The window icon is loaded at runtime, so it has to be in the installed
    # asset tree as well as the platform icon locations.
    install(FILES "${PROJECT_SOURCE_DIR}/assets/branding/icon-256.png"
            DESTINATION "${TREBLOTRON_INSTALL_BIN}/assets/branding"
            COMPONENT treblotron)

    install(FILES "${PROJECT_SOURCE_DIR}/assets/fonts/Roboto-Regular.ttf"
            DESTINATION "${TREBLOTRON_INSTALL_BIN}/assets/fonts"
            COMPONENT treblotron)

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
                DESTINATION "${TREBLOTRON_INSTALL_BIN}/${_kenney_root}/Keyboard & Mouse/Default"
            COMPONENT treblotron)
    endforeach()
    foreach(_icon IN LISTS _kenney_xbox)
        install(FILES "${PROJECT_SOURCE_DIR}/${_kenney_root}/Xbox Series/Default/${_icon}"
                DESTINATION "${TREBLOTRON_INSTALL_BIN}/${_kenney_root}/Xbox Series/Default"
            COMPONENT treblotron)
    endforeach()
endif()

# ---- Models -----------------------------------------------------------------
#
# Only configurations that actually run inference carry the 44 MB of ONNX. The
# Pi client ships none of it: its whole purpose is that the models live on the
# other end of the socket.
if(_treblotron_needs_backend)
    foreach(_asset IN LISTS _treblotron_onnx_assets)
        get_filename_component(_asset_dir "${_asset}" DIRECTORY)
        install(FILES "${PROJECT_SOURCE_DIR}/${_asset}"
                DESTINATION "${TREBLOTRON_INSTALL_BIN}/${_asset_dir}"
            COMPONENT treblotron)
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

    if(TREBLOTRON_INFER_BACKEND STREQUAL "directml")
        install(FILES ${_ort_runtime_dlls}
                DESTINATION ${TREBLOTRON_INSTALL_BIN}
                COMPONENT treblotron)

        # Only present when this tree fetched ONNX Runtime itself; a cache from
        # before the licence started being kept will not have it. Warn rather
        # than fail the configure, but do say so — shipping the binary without
        # its notice is not something to discover after publishing.
        set(_ort_license "${PROJECT_SOURCE_DIR}/external_libs/onnxruntime/LICENSE-onnxruntime.txt")
        if(EXISTS "${_ort_license}")
            install(FILES "${_ort_license}"
                    DESTINATION "${TREBLOTRON_INSTALL_BIN}/licenses"
                    COMPONENT treblotron)
        else()
            message(WARNING
                "ONNX Runtime's licence is missing from external_libs/onnxruntime. "
                "Packages built from this tree would ship the DLL without its notice. "
                "Delete external_libs/onnxruntime and re-run cmake to refetch it.")
        endif()

        if(TREBLOTRON_DIRECTML_DLL)
            # Proprietary: the licence and third-party notices travel with it.
            install(FILES
                "${PROJECT_SOURCE_DIR}/external_libs/directml/LICENSE-directml.txt"
                "${PROJECT_SOURCE_DIR}/external_libs/directml/ThirdPartyNotices.txt"
                DESTINATION "${TREBLOTRON_INSTALL_BIN}/licenses"
                COMPONENT treblotron)
        endif()
    endif()
endif()

# ---- Linux desktop integration ---------------------------------------------
#
# Installed inside the package prefix; the postinst symlinks it into
# /usr/share/applications. Putting it there directly would mean an absolute
# DESTINATION escaping CPACK_PACKAGING_INSTALL_PREFIX, which the archive
# generators handle differently from DEB.
if(UNIX AND NOT APPLE AND TREBLOTRON_BUILD_APP)
    install(FILES "${PROJECT_SOURCE_DIR}/packaging/linux/treblotron.desktop"
            DESTINATION "share/applications"
            COMPONENT treblotron)
    # Named for the product at install time, because the .desktop Icon= key and
    # the installed filename have to agree. The source file stays name-neutral
    # so a rename never has to touch the branding directory.
    string(TOLOWER "${PRJ_NAME}" _dm_icon_name)
    install(FILES "${PROJECT_SOURCE_DIR}/assets/branding/icon-256.png"
            DESTINATION "share/icons/hicolor/256x256/apps"
            RENAME "${_dm_icon_name}.png"
            COMPONENT treblotron)
endif()

install(FILES "${PROJECT_SOURCE_DIR}/LICENSE"
        DESTINATION "${TREBLOTRON_INSTALL_BIN}/licenses"
        RENAME "LICENSE-treblotron.txt"
        COMPONENT treblotron)

# ---- Toolchain DLLs the shipped libraries need ------------------------------
#
# Runs last, because it inspects what the earlier rules have already put in the
# install tree. See cmake/install_runtime_deps.cmake.in for why this is
# resolved rather than listed.
if(WIN32 AND MINGW)
    set(TREBLOTRON_INSTALLED_EXECUTABLES "")
    if(TREBLOTRON_BUILD_APP)
        list(APPEND TREBLOTRON_INSTALLED_EXECUTABLES "${PRJ_NAME}.exe")
    endif()
    if(TREBLOTRON_BUILD_SERVER)
        list(APPEND TREBLOTRON_INSTALLED_EXECUTABLES "${SERVER_NAME}.exe")
    endif()

    get_filename_component(TREBLOTRON_TOOLCHAIN_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)

    # The build output directory has to come from the target itself.
    # CMAKE_RUNTIME_OUTPUT_DIRECTORY is not set by this project — the bin/
    # subdirectory arrives from elsewhere in the configure — so reading it here
    # silently yielded an empty path and the scan found nothing at all.
    #
    # $<TARGET_FILE_DIR> is only available to install(CODE), not install(SCRIPT),
    # so the value is handed over through a variable the script then reads: both
    # run in the same scope during installation.
    list(GET TREBLOTRON_TARGETS 0 _dm_first_target)
    install(CODE
        "set(TREBLOTRON_BIN_DIR [[$<TARGET_FILE_DIR:${_dm_first_target}>]])"
        COMPONENT treblotron)

    configure_file(
        "${PROJECT_SOURCE_DIR}/cmake/install_runtime_deps.cmake.in"
        "${PROJECT_BINARY_DIR}/install_runtime_deps.cmake"
        @ONLY)
    install(SCRIPT "${PROJECT_BINARY_DIR}/install_runtime_deps.cmake"
            COMPONENT treblotron)
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

set(CPACK_PACKAGE_NAME              "Treblotron")
set(CPACK_PACKAGE_VENDOR            "ippm12")
set(CPACK_PACKAGE_VERSION           "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Automatic dart scoring and party games for a real dartboard")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Treblotron")
set(CPACK_RESOURCE_FILE_LICENSE     "${PROJECT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_CHECKSUM          SHA256)

# Package only our own component.
#
# OpenCV and the SDL family bring their own install rules — headers, static
# libraries, CMake config, haarcascade XML — and a package built without this
# filter came to 155 MB of which about 45 MB was anything a user needs. Those
# rules land in the "Unspecified" component; every rule of ours above names
# "treblotron", so listing it here is what separates the two. The install stays
# monolithic: there is no component-selection page, just a smaller payload.
set(CPACK_COMPONENTS_ALL treblotron)
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
#             C:\Program Files\Treblotron\Treblotron-0.1.0-AMD64\ and the Start
#             Menu shortcut points at a path that does not exist.
#
# Set below, next to the generator choice, rather than here where it would have
# to guess.

# What this particular build is, so the four artifacts do not collide in a
# releases directory and nobody has to guess which one they downloaded.
if(TREBLOTRON_BUILD_SERVER)
    set(_pkg_variant "Server")
elseif(TREBLOTRON_VISION_SOURCE STREQUAL "sim")
    set(_pkg_variant "Demo")
elseif(TREBLOTRON_VISION_SOURCE STREQUAL "network")
    set(_pkg_variant "Client")
else()
    set(_pkg_variant "")
endif()

if(_pkg_variant)
    set(CPACK_PACKAGE_FILE_NAME "Treblotron-${_pkg_variant}-${PROJECT_VERSION}-${CMAKE_SYSTEM_PROCESSOR}")
else()
    set(CPACK_PACKAGE_FILE_NAME "Treblotron-${PROJECT_VERSION}-${CMAKE_SYSTEM_PROCESSOR}")
endif()

if(WIN32)
    # The sim build is a demo: a folder you unzip and run, with no installer,
    # no shortcuts and nothing to uninstall. Anything that touches real
    # hardware gets a proper installed presence.
    if(TREBLOTRON_VISION_SOURCE STREQUAL "sim" AND NOT TREBLOTRON_BUILD_SERVER)
        set(CPACK_GENERATOR "ZIP")
        set(CPACK_COMPONENT_INCLUDE_TOPLEVEL_DIRECTORY 1)
    else()
        set(CPACK_GENERATOR "NSIS")
        set(CPACK_COMPONENT_INCLUDE_TOPLEVEL_DIRECTORY 0)
    endif()

    set(CPACK_NSIS_MUI_ICON   "${PROJECT_SOURCE_DIR}/assets/branding/icon.ico")
    set(CPACK_NSIS_MUI_UNIICON "${PROJECT_SOURCE_DIR}/assets/branding/icon.ico")
    set(CPACK_NSIS_PACKAGE_NAME     "Treblotron ${_pkg_variant}")
    set(CPACK_NSIS_DISPLAY_NAME     "Treblotron ${_pkg_variant} ${PROJECT_VERSION}")
    set(CPACK_NSIS_MODIFY_PATH      OFF)
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)

    # The app and the server are separate products that a Pi owner may well
    # want on the same machine -- one to play on, one serving the Pi. Left at
    # the defaults they share an install directory, a Start Menu registry key
    # and an uninstall entry, so the second install silently uninstalls the
    # first (ENABLE_UNINSTALL_BEFORE_INSTALL) and leaves one entry in
    # Add/Remove Programs pointing at a mixture of both.
    if(_pkg_variant)
        set(CPACK_PACKAGE_INSTALL_DIRECTORY     "Treblotron ${_pkg_variant}")
        set(CPACK_PACKAGE_INSTALL_REGISTRY_KEY  "Treblotron${_pkg_variant}")
    else()
        set(CPACK_PACKAGE_INSTALL_DIRECTORY     "Treblotron")
        set(CPACK_PACKAGE_INSTALL_REGISTRY_KEY  "Treblotron")
    endif()

    # CPACK_PACKAGE_EXECUTABLES is (exe-without-.exe, label) pairs, and CPack's
    # NSIS template resolves them against $INSTDIR\\bin -- which is why
    # TREBLOTRON_INSTALL_BIN is "bin" rather than a flat layout.
    if(TREBLOTRON_BUILD_SERVER)
        # Headless, but somebody still has to start it. Without a shortcut the
        # only thing in the Start Menu folder is Uninstall, which reads as a
        # broken install.
        set(CPACK_PACKAGE_EXECUTABLES "${SERVER_NAME}" "Treblotron Server")
        set(CPACK_NSIS_INSTALLED_ICON_NAME
            "${TREBLOTRON_INSTALL_BIN}\\\\${SERVER_NAME}.exe")
    else()
        set(CPACK_PACKAGE_EXECUTABLES "${PRJ_NAME}" "Treblotron")
        set(CPACK_NSIS_MUI_FINISHPAGE_RUN "${PRJ_NAME}.exe")
        set(CPACK_NSIS_INSTALLED_ICON_NAME
            "${TREBLOTRON_INSTALL_BIN}\\\\${PRJ_NAME}.exe")
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
    if(TREBLOTRON_BUILD_SERVER)
        set(CPACK_NSIS_EXTRA_INSTALL_COMMANDS "
            ExecWait 'netsh advfirewall firewall add rule name=\\\"Treblotron Server\\\" dir=in action=allow program=\\\"$INSTDIR\\\\${TREBLOTRON_INSTALL_BIN}\\\\${SERVER_NAME}.exe\\\" enable=yes'
        ")
        set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS "
            ExecWait 'netsh advfirewall firewall delete rule name=\\\"Treblotron Server\\\"'
        ")
    endif()
else()
    set(CPACK_GENERATOR "TGZ;DEB")
    # The .deb unpacks straight into /opt/treblotron, so a versioned wrapper
    # directory inside it would just be in the way. The .tar.gz gets one from
    # its own layout rules.
    set(CPACK_COMPONENT_INCLUDE_TOPLEVEL_DIRECTORY 0)
    set(CPACK_PACKAGING_INSTALL_PREFIX "/opt/treblotron")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "ippm12")
    set(CPACK_DEBIAN_PACKAGE_SECTION    "games")
    set(CPACK_DEBIAN_FILE_NAME          DEB-DEFAULT)

    # Component install has to stay ON. It is the only thing that limits the
    # payload to our own files: OpenCV and flecs carry their own install()
    # rules, which land in the Unspecified component, and a monolithic install
    # packages every component regardless of CPACK_COMPONENTS_ALL. That is how
    # share/opencv4/lbpcascades and lib/libflecs.so ended up in the .deb -- and
    # dpkg-shlibdeps then fails outright, because no Debian package provides
    # libflecs.so so it cannot derive a dependency for it.
    #
    # The component-scoped names below keep the filename the plain
    # "treblotron_0.1.0_arm64.deb" a user expects, rather than CPack's default
    # "Treblotron-treblotron_...".
    set(CPACK_DEB_COMPONENT_INSTALL              ON)
    set(CPACK_DEBIAN_TREBLOTRON_PACKAGE_NAME     "treblotron")
    set(CPACK_DEBIAN_TREBLOTRON_FILE_NAME        DEB-DEFAULT)
    set(CPACK_DEBIAN_PACKAGE_NAME                "treblotron")
    set(CPACK_DEBIAN_PACKAGE_HOMEPAGE   "https://github.com/ippm12/Treblotron")

    # /opt is outside PATH and outside the desktop menu, so the package links
    # itself into both on install and unlinks on removal.
    #
    # The scripts are generated rather than checked in with literal paths: a
    # postinst looking for a binary that install() put somewhere else does
    # nothing, silently, and the symptom is a package that installs cleanly and
    # provides no command. Substituting the same variables install() uses makes
    # that impossible.
    string(TOLOWER "${PRJ_NAME}" _dm_lower)
    set(DEB_BIN          "${CPACK_PACKAGING_INSTALL_PREFIX}/${TREBLOTRON_INSTALL_BIN}/${PRJ_NAME}")
    set(DEB_LINK         "/usr/bin/${_dm_lower}")
    set(DEB_DESKTOP      "${CPACK_PACKAGING_INSTALL_PREFIX}/share/applications/${_dm_lower}.desktop")
    set(DEB_DESKTOP_LINK "/usr/share/applications/${_dm_lower}.desktop")

    foreach(_dm_script postinst prerm)
        configure_file(
            "${PROJECT_SOURCE_DIR}/packaging/linux/${_dm_script}.in"
            "${PROJECT_BINARY_DIR}/packaging/${_dm_script}"
            @ONLY)
        # dpkg refuses a control script that is not executable, and
        # configure_file does not carry the source permissions over.
        file(CHMOD "${PROJECT_BINARY_DIR}/packaging/${_dm_script}"
             PERMISSIONS
                 OWNER_READ OWNER_WRITE OWNER_EXECUTE
                 GROUP_READ GROUP_EXECUTE
                 WORLD_READ WORLD_EXECUTE)
    endforeach()

    # Component-scoped, because that is the only form CPack reads once
    # CPACK_DEB_COMPONENT_INSTALL is on -- the unscoped variable is ignored, and
    # the package ships with no maintainer scripts at all.
    set(CPACK_DEBIAN_TREBLOTRON_PACKAGE_CONTROL_EXTRA
        "${PROJECT_BINARY_DIR}/packaging/postinst;${PROJECT_BINARY_DIR}/packaging/prerm")
    # Left to dpkg-shlibdeps: the Pi client links system OpenCV and SDL
    # transitively and hand-listing them goes stale the first time a dependency
    # moves.
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
endif()

include(CPack)
