message("Start including external libraries")

# spdlog
add_subdirectory(spdlog)
target_link_libraries(${PRJ_NAME} PRIVATE spdlog::spdlog)

# SDL
add_subdirectory(SDL)

# Flecs
add_subdirectory(flecs)
target_link_libraries(${PRJ_NAME} PRIVATE flecs::flecs_static)

# SDL_ttf (TrueType font rendering)
set(SDLTTF_VENDORED ON CACHE BOOL "Use vendored freetype/harfbuzz" FORCE)
add_subdirectory(SDL_ttf)
target_link_libraries(${PRJ_NAME} PRIVATE SDL3_ttf::SDL3_ttf)

# SDL_image (PNG/JPG image loading)
add_subdirectory(SDL_image)
target_link_libraries(${PRJ_NAME} PRIVATE SDL3_image::SDL3_image)

# SDL3 linked last — SDL_ttf and SDL_image static libs need its symbols,
# and GNU ld resolves left-to-right
target_link_libraries(${PRJ_NAME} PRIVATE SDL3::SDL3)

# OpenCV (minimal build — only core, imgproc, calib3d, videoio, imgcodecs)
set(BUILD_LIST "core,imgproc,features2d,flann,calib3d,videoio,imgcodecs" CACHE STRING "" FORCE)
set(BUILD_SHARED_LIBS_SAVED ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_PERF_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_opencv_apps OFF CACHE BOOL "" FORCE)
set(BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(BUILD_JAVA OFF CACHE BOOL "" FORCE)
set(BUILD_opencv_python3 OFF CACHE BOOL "" FORCE)
set(WITH_CUDA OFF CACHE BOOL "" FORCE)
set(WITH_OPENCL OFF CACHE BOOL "" FORCE)
set(WITH_TIFF OFF CACHE BOOL "" FORCE)
set(WITH_WEBP OFF CACHE BOOL "" FORCE)
set(WITH_OPENJPEG OFF CACHE BOOL "" FORCE)
set(WITH_JASPER OFF CACHE BOOL "" FORCE)
set(WITH_OPENEXR OFF CACHE BOOL "" FORCE)
set(OPENCV_GENERATE_PKGCONFIG OFF CACHE BOOL "" FORCE)
# Always build OpenCV in Release mode — Debug mode disables compiler
# optimizations and NEON auto-vectorization, making cv::remap ~30x slower.
set(CMAKE_C_FLAGS_SAVED "${CMAKE_C_FLAGS}")
set(CMAKE_CXX_FLAGS_SAVED "${CMAKE_CXX_FLAGS}")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS_RELEASE}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS_RELEASE}")
set(CMAKE_BUILD_TYPE_SAVED "${CMAKE_BUILD_TYPE}")
set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
add_subdirectory(opencv)
set(CMAKE_BUILD_TYPE "${CMAKE_BUILD_TYPE_SAVED}" CACHE STRING "" FORCE)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS_SAVED}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS_SAVED}")
target_link_libraries(${PRJ_NAME} PRIVATE opencv_core opencv_imgproc opencv_calib3d opencv_videoio opencv_imgcodecs)
# OpenCV's add_subdirectory doesn't propagate include dirs properly — add them explicitly
target_include_directories(${PRJ_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/opencv/modules/core/include
    ${CMAKE_CURRENT_SOURCE_DIR}/opencv/modules/imgproc/include
    ${CMAKE_CURRENT_SOURCE_DIR}/opencv/modules/features2d/include
    ${CMAKE_CURRENT_SOURCE_DIR}/opencv/modules/flann/include
    ${CMAKE_CURRENT_SOURCE_DIR}/opencv/modules/calib3d/include
    ${CMAKE_CURRENT_SOURCE_DIR}/opencv/modules/videoio/include
    ${CMAKE_CURRENT_SOURCE_DIR}/opencv/modules/imgcodecs/include
    ${CMAKE_BINARY_DIR}  # generated headers (opencv2/cvconfig.h, opencv2/opencv_modules.hpp)
)
