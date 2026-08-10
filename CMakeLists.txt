cmake_minimum_required(VERSION 3.22.1)

# CMAKE_CXX_STANDARD set as a directory variable would leak into OpenCV /
# SDL subprojects and interact badly with their own compiler-feature
# checks. We apply cxx_std_23 to our targets explicitly via
# target_compile_features() below instead.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(PRJ_NAME Dart_Lens)
set(SERVER_NAME Dart_Lens_Server)

# ============================================================================
# Configuration
# ============================================================================
#
# Two independent axes:
#
#   DARTLENS_VISION_SOURCE  where the game gets dart events from
#   DARTLENS_INFER_BACKEND  what actually executes a forward pass
#
# They're separate because the same inference backend serves two very
# different binaries: the game running detection locally, and the headless
# server running it on behalf of a remote client. Typical combinations:
#
#   dev workstation      VISION_SOURCE=sim      INFER_BACKEND=none
#   Jetson, standalone   VISION_SOURCE=local    INFER_BACKEND=tensorrt
#   Raspberry Pi + server VISION_SOURCE=network INFER_BACKEND=none
#   inference server     BUILD_APP=OFF BUILD_SERVER=ON INFER_BACKEND=cpu|tensorrt

option(DARTLENS_BUILD_APP    "Build the Dart_Lens game application"    ON)
option(DARTLENS_BUILD_SERVER "Build the headless inference server"     OFF)
option(DARTLENS_SHOW_FPS     "Show framerate counter in the top right" ON)

set(DARTLENS_VISION_SOURCE "sim"  CACHE STRING "Vision source: sim|hailo|local|network")
set(DARTLENS_INFER_BACKEND "none" CACHE STRING "Inference backend: none|tensorrt|directml|cpu")
set_property(CACHE DARTLENS_VISION_SOURCE PROPERTY STRINGS sim hailo local network)
set_property(CACHE DARTLENS_INFER_BACKEND PROPERTY STRINGS none tensorrt directml cpu)

# Pinned so a silent upstream bump can't change inference behaviour underfoot.
set(DARTLENS_ORT_VERSION "1.24.4" CACHE STRING "ONNX Runtime version for the directml backend")
set(DARTLENS_DIRECTML_VERSION "1.15.4" CACHE STRING "DirectML redistributable version")

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
option(DARTLENS_FETCH_DIRECTML "Ship the DirectML redistributable instead of using the Windows copy" OFF)

# Back-compat with the previous boolean flags so existing command lines,
# .vscode configs and build caches keep working.
#
# Two rules keep a legacy flag from outliving its usefulness:
#
#   - it only applies when the new variable is still at its default, so an
#     explicit -DDARTLENS_VISION_SOURCE always wins;
#   - it is removed from the cache once honoured, so it cannot silently steer
#     every future configure of the same build directory.
#
# Both matter because the old flags live in existing build caches. Without
# them, reconfiguring a build directory that once had DARTLENS_USE_HAILO=ON
# would keep demanding HailoRT no matter what you asked for — which is exactly
# what happens when a Pi's accelerator is removed and the build tree isn't.
macro(_dartlens_retire_flag legacy explanation)
    if(${legacy})
        if(_dartlens_source_is_default)
            message(DEPRECATION "${legacy} is deprecated — ${explanation}")
            set(_dartlens_legacy_applied TRUE)
        else()
            message(WARNING
                "Ignoring stale ${legacy} from the build cache: "
                "DARTLENS_VISION_SOURCE=${DARTLENS_VISION_SOURCE} was requested explicitly. "
                "Clearing ${legacy} so it stops interfering.")
        endif()
    endif()
    unset(${legacy} CACHE)
endmacro()

set(_dartlens_source_is_default FALSE)
if(DARTLENS_VISION_SOURCE STREQUAL "sim")
    set(_dartlens_source_is_default TRUE)
endif()

set(_dartlens_legacy_applied FALSE)
_dartlens_retire_flag(DARTLENS_USE_HAILO "use -DDARTLENS_VISION_SOURCE=hailo")
if(_dartlens_legacy_applied)
    set(DARTLENS_VISION_SOURCE "hailo" CACHE STRING "Vision source" FORCE)
endif()

set(_dartlens_legacy_applied FALSE)
_dartlens_retire_flag(DARTLENS_USE_TENSORRT
    "use -DDARTLENS_VISION_SOURCE=local -DDARTLENS_INFER_BACKEND=tensorrt")
if(_dartlens_legacy_applied)
    set(DARTLENS_VISION_SOURCE "local"    CACHE STRING "Vision source"      FORCE)
    set(DARTLENS_INFER_BACKEND "tensorrt" CACHE STRING "Inference backend"  FORCE)
endif()

# Never read; retired so it cannot confuse anyone reading a stale cache.
unset(DARTLENS_USE_SIM CACHE)

if(NOT DARTLENS_VISION_SOURCE MATCHES "^(sim|hailo|local|network)$")
    message(FATAL_ERROR "DARTLENS_VISION_SOURCE must be sim, hailo, local or network "
                        "(got '${DARTLENS_VISION_SOURCE}')")
endif()
if(NOT DARTLENS_INFER_BACKEND MATCHES "^(none|tensorrt|directml|cpu)$")
    message(FATAL_ERROR "DARTLENS_INFER_BACKEND must be none, tensorrt, directml or cpu "
                        "(got '${DARTLENS_INFER_BACKEND}')")
endif()
if(DARTLENS_INFER_BACKEND STREQUAL "directml" AND NOT WIN32)
    message(FATAL_ERROR "The directml backend is Windows-only (DirectML is a DirectX 12 "
                        "component). Use tensorrt on Jetson, or cpu elsewhere.")
endif()
if(NOT DARTLENS_BUILD_APP AND NOT DARTLENS_BUILD_SERVER)
    message(FATAL_ERROR "Nothing to build — enable DARTLENS_BUILD_APP and/or DARTLENS_BUILD_SERVER")
endif()

# The pieces that need a real backend: the local vision source, and the server.
set(_dartlens_needs_backend OFF)
if(DARTLENS_BUILD_APP AND DARTLENS_VISION_SOURCE STREQUAL "local")
    set(_dartlens_needs_backend ON)
endif()
if(DARTLENS_BUILD_SERVER)
    set(_dartlens_needs_backend ON)
endif()
if(_dartlens_needs_backend AND DARTLENS_INFER_BACKEND STREQUAL "none")
    message(FATAL_ERROR
        "This configuration runs the detection models, so it needs an inference "
        "backend. Add -DDARTLENS_INFER_BACKEND=tensorrt (Jetson) or =cpu (OpenCV DNN).")
endif()

# CUDA has to be declared in project(LANGUAGES ...) — more robust on
# CMake 3.22 / JetPack than enable_language(CUDA) after project().
set(_DARTLENS_LANGUAGES C CXX)
if(DARTLENS_INFER_BACKEND STREQUAL "tensorrt")
    if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
        # Orin Nano Super: Ampere, compute capability 8.7.
        set(CMAKE_CUDA_ARCHITECTURES 87)
    endif()
    list(APPEND _DARTLENS_LANGUAGES CUDA)
endif()

project(${PRJ_NAME} LANGUAGES ${_DARTLENS_LANGUAGES})

message(STATUS "DartLens: app=${DARTLENS_BUILD_APP} server=${DARTLENS_BUILD_SERVER} "
               "vision_source=${DARTLENS_VISION_SOURCE} backend=${DARTLENS_INFER_BACKEND}")


# ============================================================================
# Module helper
# ============================================================================
#
# A module is a directory with public headers in <mod>/inc_public/<mod>/ and
# flat implementation files in <mod>/*.cpp. Globbed with CONFIGURE_DEPENDS so
# there are no source lists to maintain. Two targets share this, which is why
# it's a function rather than the inline loop it used to be.

function(dartlens_add_module target module)
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

# Shared settings every DartLens target wants.
function(dartlens_configure_target target)
    target_compile_features(${target} PRIVATE cxx_std_23)
    target_compile_options(${target} PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Werror>
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
    )
    # Common include should be available to everyone
    target_include_directories(${target} PUBLIC ${PROJECT_SOURCE_DIR}/inc_common)
    file(GLOB INC_COMMON_SOURCES CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/inc_common/*.h"
        "${PROJECT_SOURCE_DIR}/inc_common/*.hpp"
        "${PROJECT_SOURCE_DIR}/inc_common/*.cpp")
    target_sources(${target} PRIVATE ${INC_COMMON_SOURCES})

    if(DARTLENS_INFER_BACKEND STREQUAL "cpu")
        target_compile_definitions(${target} PRIVATE DARTLENS_INFER_CPU)
    elseif(DARTLENS_INFER_BACKEND STREQUAL "tensorrt")
        target_compile_definitions(${target} PRIVATE DARTLENS_INFER_TENSORRT)
    elseif(DARTLENS_INFER_BACKEND STREQUAL "directml")
        target_compile_definitions(${target} PRIVATE DARTLENS_INFER_DIRECTML)
    endif()
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
    ${PROJECT_SOURCE_DIR}/vision/hailo_vision_source.cpp
    ${PROJECT_SOURCE_DIR}/vision/local_vision_source.cpp
    ${PROJECT_SOURCE_DIR}/vision/network_vision_source.cpp)
set(_active_vision_source
    ${PROJECT_SOURCE_DIR}/vision/${DARTLENS_VISION_SOURCE}_vision_source.cpp)
foreach(_src IN LISTS _all_vision_sources)
    if(NOT _src STREQUAL _active_vision_source OR NOT DARTLENS_BUILD_APP)
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
    if(NOT _src STREQUAL "${_backend_source_${DARTLENS_INFER_BACKEND}}")
        set_source_files_properties(${_src} PROPERTIES HEADER_FILE_ONLY TRUE)
    endif()
endforeach()
if(DARTLENS_INFER_BACKEND STREQUAL "none")
    set_source_files_properties(${PROJECT_SOURCE_DIR}/detect/dart_detector.cpp
        PROPERTIES HEADER_FILE_ONLY TRUE)
endif()


# ============================================================================
# Game application
# ============================================================================

if(DARTLENS_BUILD_APP)
    add_executable(${PRJ_NAME} startup.cpp)
    dartlens_configure_target(${PRJ_NAME})

    set(APP_MODULES_LIST "debug" "frame" "dart" "players" "game_lib" "games"
                         "detect" "net" "vision")
    foreach(lib IN LISTS APP_MODULES_LIST)
        message("Including ${lib} library")
        dartlens_add_module(${PRJ_NAME} ${lib})
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

    string(TOUPPER ${DARTLENS_VISION_SOURCE} _vision_source_upper)
    target_compile_definitions(${PRJ_NAME} PRIVATE DARTLENS_USE_${_vision_source_upper})

    if(DARTLENS_VISION_SOURCE STREQUAL "local")
        # The capture threads pre-compute seg input and the detector publishes
        # its own warped frames — see camera_api.cpp.
        target_compile_definitions(${PRJ_NAME} PRIVATE DARTLENS_HAVE_LOCAL_INFERENCE)
    endif()

    if(DARTLENS_VISION_SOURCE STREQUAL "network")
        # Forward the cameras' own MJPEG instead of decoding and re-encoding it.
        target_compile_definitions(${PRJ_NAME} PRIVATE DARTLENS_PASSTHROUGH_CAPTURE)
    endif()

    if(DARTLENS_VISION_SOURCE STREQUAL "hailo")
        find_package(HailoRT REQUIRED)
        target_link_libraries(${PRJ_NAME} PRIVATE HailoRT::libhailort)
    endif()

    if(DARTLENS_SHOW_FPS)
        target_compile_definitions(${PRJ_NAME} PRIVATE DARTLENS_SHOW_FPS)
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

if(DARTLENS_BUILD_SERVER)
    add_executable(${SERVER_NAME} server/server_main.cpp)
    dartlens_configure_target(${SERVER_NAME})

    set(SERVER_MODULES_LIST "debug" "dart" "detect" "net" "server")
    foreach(lib IN LISTS SERVER_MODULES_LIST)
        message("Including ${lib} library (server)")
        dartlens_add_module(${SERVER_NAME} ${lib})
    endforeach()
endif()


# ============================================================================
# Backend dependencies
# ============================================================================

set(DARTLENS_TARGETS "")
if(DARTLENS_BUILD_APP)
    list(APPEND DARTLENS_TARGETS ${PRJ_NAME})
endif()
if(DARTLENS_BUILD_SERVER)
    list(APPEND DARTLENS_TARGETS ${SERVER_NAME})
endif()

# Winsock. Both targets link the net module — the server always, the app
# whenever the network vision source is in play — and ws2_32 is harmless
# otherwise.
if(WIN32)
    foreach(_t IN LISTS DARTLENS_TARGETS)
        target_link_libraries(${_t} PRIVATE ws2_32)
    endforeach()
endif()

if(DARTLENS_INFER_BACKEND STREQUAL "directml")
    # ONNX Runtime ships as MSVC-built binaries, but that is not a problem
    # here: ort_backend.cpp reaches the library through GetProcAddress on the
    # single exported C function OrtGetApiBase, so there is no link-time
    # dependency and no C++ ABI to match. We only need the headers at build
    # time and the DLLs next to the executable at run time.
    #
    # DirectML.dll itself is NOT fetched — Windows ships one in System32 (1.15.5
    # on this machine, newer than the redistributable), and the loader finds it
    # automatically. Point DARTLENS_DIRECTML_DLL at a copy from the
    # Microsoft.AI.DirectML package if a target machine's inbox version is too
    # old for the pinned ONNX Runtime.
    set(_ort_root "${PROJECT_SOURCE_DIR}/external_libs/onnxruntime")
    set(_ort_stamp "${_ort_root}/.version-${DARTLENS_ORT_VERSION}")

    if(NOT EXISTS "${_ort_stamp}")
        set(_ort_url "https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime.directml/${DARTLENS_ORT_VERSION}/microsoft.ml.onnxruntime.directml.${DARTLENS_ORT_VERSION}.nupkg")
        set(_ort_zip "${CMAKE_BINARY_DIR}/ort.nupkg")
        message(STATUS "Fetching ONNX Runtime ${DARTLENS_ORT_VERSION} (DirectML) — ~12 MB, once")
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
        file(WRITE "${_ort_stamp}" "${DARTLENS_ORT_VERSION}\n")
        message(STATUS "ONNX Runtime unpacked into ${_ort_root}")
    else()
        message(STATUS "Using cached ONNX Runtime ${DARTLENS_ORT_VERSION} in ${_ort_root}")
    endif()

    if(NOT EXISTS "${_ort_root}/include/onnxruntime_c_api.h")
        message(FATAL_ERROR "ONNX Runtime headers missing from ${_ort_root}/include — "
                            "delete ${_ort_root} and re-run cmake to refetch.")
    endif()

    # Optionally fetch the DirectML redistributable. Cached the same way as ORT.
    if(DARTLENS_FETCH_DIRECTML AND NOT DARTLENS_DIRECTML_DLL)
        set(_dml_dir "${PROJECT_SOURCE_DIR}/external_libs/directml")
        set(_dml_stamp "${_dml_dir}/.version-${DARTLENS_DIRECTML_VERSION}")
        if(NOT EXISTS "${_dml_stamp}")
            set(_dml_url "https://api.nuget.org/v3-flatcontainer/microsoft.ai.directml/${DARTLENS_DIRECTML_VERSION}/microsoft.ai.directml.${DARTLENS_DIRECTML_VERSION}.nupkg")
            set(_dml_zip "${CMAKE_BINARY_DIR}/directml.nupkg")
            message(STATUS "Fetching DirectML ${DARTLENS_DIRECTML_VERSION} — ~200 MB, once "
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
            file(WRITE "${_dml_stamp}" "${DARTLENS_DIRECTML_VERSION}\n")
        endif()
        set(DARTLENS_DIRECTML_DLL "${_dml_dir}/DirectML.dll")
    endif()

    file(GLOB _ort_runtime_dlls "${_ort_root}/bin/*.dll")
    if(DARTLENS_DIRECTML_DLL)
        list(APPEND _ort_runtime_dlls "${DARTLENS_DIRECTML_DLL}")
        message(STATUS "Shipping DirectML.dll from ${DARTLENS_DIRECTML_DLL}")
    else()
        message(STATUS "Using the DirectML.dll that ships with Windows "
                       "(set DARTLENS_FETCH_DIRECTML=ON if the backend reports a CPU fallback)")
    endif()

    foreach(_t IN LISTS DARTLENS_TARGETS)
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

if(DARTLENS_INFER_BACKEND STREQUAL "tensorrt")
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

    foreach(_t IN LISTS DARTLENS_TARGETS)
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

if(_dartlens_needs_backend)
    set(_dartlens_onnx_assets
        detect/models/multicam_unet_ar/multicam_unet_ar.onnx
        detect/models/multicam_unet_ar/dart_seg_unet.onnx
        detect/models/palm_detection/blazepalm.onnx
        detect/models/palm_detection/hand_landmark.onnx
    )
    foreach(_t IN LISTS DARTLENS_TARGETS)
        foreach(_asset IN LISTS _dartlens_onnx_assets)
            get_filename_component(_asset_dir ${_asset} DIRECTORY)
            add_custom_command(TARGET ${_t} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory
                    $<TARGET_FILE_DIR:${_t}>/${_asset_dir}
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${PROJECT_SOURCE_DIR}/${_asset}
                    $<TARGET_FILE_DIR:${_t}>/${_asset})
        endforeach()
    endforeach()
endif()


# ============================================================================
# External libraries
# ============================================================================

set(EXTERNAL_LIBS ${PROJECT_SOURCE_DIR}/external_libs)
include_directories(${EXTERNAL_LIBS})
add_subdirectory(${EXTERNAL_LIBS})

# Copy DLLs to output directory (for Windows)
if(WIN32 AND DARTLENS_BUILD_APP)
    foreach(_dll SDL3::SDL3 SDL3_ttf::SDL3_ttf spdlog::spdlog SDL3_image::SDL3_image)
        add_custom_command(TARGET ${PRJ_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:${_dll}>
            $<TARGET_FILE_DIR:${PRJ_NAME}>
        )
    endforeach()
endif()
if(WIN32 AND DARTLENS_BUILD_SERVER)
    add_custom_command(TARGET ${SERVER_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:spdlog::spdlog>
        $<TARGET_FILE_DIR:${SERVER_NAME}>
    )
endif()
