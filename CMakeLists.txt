cmake_minimum_required(VERSION 3.22.1)

# CMAKE_CXX_STANDARD set as a directory variable would leak into OpenCV /
# SDL subprojects and interact badly with their own compiler-feature
# checks. We apply cxx_std_23 to our target explicitly via
# target_compile_features() below instead.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(PRJ_NAME Dart_Lens)

# DARTLENS_USE_TENSORRT comes in via -D on the cmake command line and is
# already in the cache at this point — so we can read it before the
# option() calls below to decide whether to enable CUDA as a project
# language. Declaring CUDA in project(LANGUAGES ...) is more robust on
# CMake 3.22 / JetPack than calling enable_language(CUDA) after project().
set(_DARTLENS_LANGUAGES C CXX)
if(DARTLENS_USE_TENSORRT)
    if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
        # Orin Nano Super: Ampere, compute capability 8.7.
        set(CMAKE_CUDA_ARCHITECTURES 87)
    endif()
    list(APPEND _DARTLENS_LANGUAGES CUDA)
endif()

project(${PRJ_NAME} LANGUAGES ${_DARTLENS_LANGUAGES})

option(DARTLENS_USE_SIM "Use simulated dart vision source instead of cameras" ON)
option(DARTLENS_USE_HAILO "Use HAILO-accelerated dart detection (requires HailoRT)" OFF)
option(DARTLENS_USE_TENSORRT "Use TensorRT-accelerated dart detection (Jetson)" OFF)
option(DARTLENS_SHOW_FPS "Show framerate counter in the top right" ON)

# Count enabled vision-source flags — they're mutually exclusive.
set(_dartlens_vision_sources 0)
if(DARTLENS_USE_SIM)
    math(EXPR _dartlens_vision_sources "${_dartlens_vision_sources} + 1")
endif()
if(DARTLENS_USE_HAILO)
    math(EXPR _dartlens_vision_sources "${_dartlens_vision_sources} + 1")
endif()
if(DARTLENS_USE_TENSORRT)
    math(EXPR _dartlens_vision_sources "${_dartlens_vision_sources} + 1")
endif()
if(_dartlens_vision_sources GREATER 1)
    message(FATAL_ERROR "DARTLENS_USE_SIM, DARTLENS_USE_HAILO, and DARTLENS_USE_TENSORRT are mutually exclusive — enable only one vision source.")
endif()

set(MODULES_LIST "debug" "frame" "dart" "players" "game_lib" "games" "vision")

add_executable(${PRJ_NAME} startup.cpp)

target_compile_features(${PRJ_NAME} PRIVATE cxx_std_23)

# Enable warnings and treat them as errors
target_compile_options(${PRJ_NAME} PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)

# Common include should be available to everyone
target_include_directories(${PRJ_NAME} PUBLIC ${PROJECT_SOURCE_DIR}/inc_common)
# All inc-common needs to be included as sources
file(GLOB INC_COMMON_SOURCES CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/inc_common/*.h" "${PROJECT_SOURCE_DIR}/inc_common/*.hpp" "${PROJECT_SOURCE_DIR}/inc_common/*.cpp")
target_sources(${PRJ_NAME} PRIVATE ${INC_COMMON_SOURCES})

# Add all of the subproject directories
foreach(lib IN LISTS MODULES_LIST)
    message("Including ${lib} library")
    target_include_directories(${PRJ_NAME} PUBLIC ${PROJECT_SOURCE_DIR}/${lib}/inc_public)
    # Collect all source files
    file(GLOB LIB_SOURCES CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/${lib}/*.h" "${PROJECT_SOURCE_DIR}/${lib}/*.hpp" "${PROJECT_SOURCE_DIR}/${lib}/*.cpp")
    file(GLOB_RECURSE LIB_PUBLIC_SOURCES CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/${lib}/inc_public/${lib}/*.h" "${PROJECT_SOURCE_DIR}/${lib}/inc_public/${lib}/*.hpp")
    target_sources(${PRJ_NAME} PRIVATE ${LIB_PUBLIC_SOURCES} ${LIB_SOURCES})
endforeach()

# Auto-discover game subdirectories (each game is a self-contained folder under games/)
file(GLOB GAME_SUBDIRS LIST_DIRECTORIES true CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/games/*/")
foreach(game_dir IN LISTS GAME_SUBDIRS)
    if(IS_DIRECTORY ${game_dir})
        get_filename_component(game_name ${game_dir} NAME)
        # Skip inc_public — it's handled by the module loop above
        if(NOT game_name STREQUAL "inc_public")
            message("  Including game: ${game_name}")
            file(GLOB GAME_SRCS CONFIGURE_DEPENDS "${game_dir}/*.h" "${game_dir}/*.hpp" "${game_dir}/*.cpp")
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

# Vision source selection: only one of sim/hailo/tensorrt is compiled in.
# The other source files are marked header-only so they're skipped by the
# compiler but still tracked by CMake (keeps the glob-based source list honest).
if(DARTLENS_USE_SIM)
    target_compile_definitions(${PRJ_NAME} PRIVATE DARTLENS_USE_SIM)
    set_source_files_properties(
        ${PROJECT_SOURCE_DIR}/vision/hailo_vision_source.cpp
        ${PROJECT_SOURCE_DIR}/vision/tensorrt_vision_source.cpp
        PROPERTIES HEADER_FILE_ONLY TRUE)
endif()

if(DARTLENS_USE_HAILO)
    target_compile_definitions(${PRJ_NAME} PRIVATE DARTLENS_USE_HAILO)
    set_source_files_properties(
        ${PROJECT_SOURCE_DIR}/vision/sim_vision_source.cpp
        ${PROJECT_SOURCE_DIR}/vision/tensorrt_vision_source.cpp
        PROPERTIES HEADER_FILE_ONLY TRUE)

    find_package(HailoRT REQUIRED)
    target_link_libraries(${PRJ_NAME} PRIVATE HailoRT::libhailort)
endif()

if(DARTLENS_USE_TENSORRT)
    target_compile_definitions(${PRJ_NAME} PRIVATE DARTLENS_USE_TENSORRT)
    set_source_files_properties(
        ${PROJECT_SOURCE_DIR}/vision/sim_vision_source.cpp
        ${PROJECT_SOURCE_DIR}/vision/hailo_vision_source.cpp
        PROPERTIES HEADER_FILE_ONLY TRUE)

    # CUDA runtime — enable_language(CUDA) above already populates CUDAToolkit_*.
    find_package(CUDAToolkit REQUIRED)

    # TensorRT headers + libs. JetPack installs them under the standard
    # /usr/include/aarch64-linux-gnu and /usr/lib/aarch64-linux-gnu paths;
    # we still search explicitly so a missing install produces a clear
    # error pointing at the apt package.
    find_path(TENSORRT_INCLUDE_DIR NvInfer.h
        HINTS /usr/include/aarch64-linux-gnu /usr/include
        DOC "TensorRT include directory")
    find_library(TENSORRT_INFER_LIB       nvinfer
        HINTS /usr/lib/aarch64-linux-gnu /usr/lib
        DOC "TensorRT libnvinfer")
    find_library(TENSORRT_ONNXPARSER_LIB  nvonnxparser
        HINTS /usr/lib/aarch64-linux-gnu /usr/lib
        DOC "TensorRT libnvonnxparser")
    if(NOT TENSORRT_INCLUDE_DIR OR NOT TENSORRT_INFER_LIB OR NOT TENSORRT_ONNXPARSER_LIB)
        message(FATAL_ERROR
            "TensorRT not found. On JetPack: sudo apt install tensorrt "
            "tensorrt-dev libnvonnxparsers-dev libcudnn9-dev")
    endif()

    target_include_directories(${PRJ_NAME} PRIVATE ${TENSORRT_INCLUDE_DIR})
    target_link_libraries(${PRJ_NAME} PRIVATE
        CUDA::cudart
        ${TENSORRT_INFER_LIB}
        ${TENSORRT_ONNXPARSER_LIB})

    # Ship the ONNX model next to the executable so the TRT source can find
    # it on first run (or fall back to the cached .trt).
    add_custom_command(TARGET ${PRJ_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${PROJECT_SOURCE_DIR}/vision/multicam_unet_ar
        $<TARGET_FILE_DIR:${PRJ_NAME}>/vision/multicam_unet_ar)

    # Same for the BlazePalm presence detector — the TRT source builds a
    # second engine from this ONNX on first run and caches the .trt next
    # to it. See vision/palm_detection/README.md for export instructions.
    add_custom_command(TARGET ${PRJ_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${PROJECT_SOURCE_DIR}/vision/palm_detection
        $<TARGET_FILE_DIR:${PRJ_NAME}>/vision/palm_detection)
endif()

if(NOT DARTLENS_USE_SIM AND NOT DARTLENS_USE_HAILO AND NOT DARTLENS_USE_TENSORRT)
    set_source_files_properties(
        ${PROJECT_SOURCE_DIR}/vision/sim_vision_source.cpp
        ${PROJECT_SOURCE_DIR}/vision/hailo_vision_source.cpp
        ${PROJECT_SOURCE_DIR}/vision/tensorrt_vision_source.cpp
        PROPERTIES HEADER_FILE_ONLY TRUE)
endif()

if(DARTLENS_SHOW_FPS)
    target_compile_definitions(${PRJ_NAME} PRIVATE DARTLENS_SHOW_FPS)
endif()

# Add all of the external libraries
set(EXTERNAL_LIBS ${PROJECT_SOURCE_DIR}/external_libs)
include_directories(${EXTERNAL_LIBS})
add_subdirectory(${EXTERNAL_LIBS})

# Copy DLLs to output directory (for Windows)
if(WIN32)
    add_custom_command(TARGET ${PRJ_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:SDL3::SDL3>
        $<TARGET_FILE_DIR:${PRJ_NAME}>
    )
    add_custom_command(TARGET ${PRJ_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:SDL3_ttf::SDL3_ttf>
        $<TARGET_FILE_DIR:${PRJ_NAME}>
    )
    add_custom_command(TARGET ${PRJ_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:spdlog::spdlog>
        $<TARGET_FILE_DIR:${PRJ_NAME}>
    )
    add_custom_command(TARGET ${PRJ_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:SDL3_image::SDL3_image>
        $<TARGET_FILE_DIR:${PRJ_NAME}>
    )
endif()

# Copy assets to build output directory
add_custom_command(TARGET ${PRJ_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${PROJECT_SOURCE_DIR}/assets
    $<TARGET_FILE_DIR:${PRJ_NAME}>/assets
)
