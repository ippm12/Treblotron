message("Start including external libraries")

# spdlog
add_subdirectory(spdlog)
target_link_libraries(${PRJ_NAME} PRIVATE spdlog::spdlog)

# SDL
add_subdirectory(SDL)
target_link_libraries(${PRJ_NAME} PRIVATE SDL3::SDL3)

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

# Add more libraries here
