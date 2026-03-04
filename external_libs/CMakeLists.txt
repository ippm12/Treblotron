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

# Add more libraries here
