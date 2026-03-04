cmake_minimum_required(VERSION 4.0.2)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(PRJ_NAME Dart_Lens)

project(${PRJ_NAME})

set(MODULES_LIST "debug" "frame" "game_lib"  "games")

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
    file(GLOB LIB_PUBLIC_SOURCES CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/${lib}/inc_public/${lib}/*.h" "${PROJECT_SOURCE_DIR}/${lib}/inc_public/${lib}*.hpp")
    target_sources(${PRJ_NAME} PRIVATE ${LIB_PUBLIC_SOURCES} ${LIB_SOURCES})
endforeach()

# Add all of the external libraries
set(EXTERNAL_LIBS ${PROJECT_SOURCE_DIR}/external_libs)
include_directories(${EXTERNAL_LIBS})
add_subdirectory(${EXTERNAL_LIBS})

# Copy SDL3 DLL to output directory (for Windows)
if(WIN32)
    add_custom_command(TARGET ${PRJ_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:SDL3::SDL3>
        $<TARGET_FILE_DIR:${PRJ_NAME}>
    )
endif()
