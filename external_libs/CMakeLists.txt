message("Start including external libraries")

# spdlog
add_subdirectory(spdlog)
target_link_libraries(${PRJ_NAME} PRIVATE spdlog::spdlog)

# Add more libraries here
