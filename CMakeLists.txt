cmake_minimum_required(VERSION 3.31.6)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(PRJ_NAME Dart_Lens)

project(${PRJ_NAME})

option(DARTLENS_USE_SIM "Use simulated dart vision source instead of cameras" ON)
option(DARTLENS_SHOW_FPS "Show framerate counter in the top right" ON)

set(MODULES_LIST "debug" "frame" "dart" "players" "game_lib" "games" "vision")

add_executable(${PRJ_NAME} startup.cpp)

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

# Sim vision compile definition
if(DARTLENS_USE_SIM)
    target_compile_definitions(${PRJ_NAME} PRIVATE DARTLENS_USE_SIM)
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
