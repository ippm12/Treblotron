message("Start including external libraries")

# spdlog
add_subdirectory(spdlog)
target_link_libraries(${PRJ_NAME} PRIVATE spdlog::spdlog)

# SDL
add_subdirectory(SDL)
target_link_libraries(${PRJ_NAME} PRIVATE SDL3::SDL3)

# Add more libraries here
