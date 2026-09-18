# Gameplay sources are discovered by the host's generic games loader.
# Stage data on every build, including edits that require no C++ relink.
add_custom_target(minigolf_assets
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:${PRJ_NAME}>/assets/games/minigolf"
    COMMENT "Staging Mini Golf assets")
add_dependencies(${PRJ_NAME} minigolf_assets)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/assets/"
    DESTINATION "${TREBLOTRON_INSTALL_BIN}/assets/games/minigolf"
    COMPONENT treblotron)

option(TREBLOTRON_BUILD_COURSE_EDITOR "Build the desktop Mini Golf course editor" ON)
if(TREBLOTRON_BUILD_COURSE_EDITOR)
    add_executable(TreblotronCourseEditor
        editor/main.cpp editor/editor.cpp editor/document.cpp
        ${TREBLOTRON_WIN_RESOURCES})
    treblotron_configure_target(TreblotronCourseEditor)
    target_link_libraries(TreblotronCourseEditor PRIVATE TreblotronRuntime)
    # The host stages shared fonts and runtime libraries for both executables.
    add_dependencies(TreblotronCourseEditor ${PRJ_NAME})
    install(TARGETS TreblotronCourseEditor
        RUNTIME DESTINATION ${TREBLOTRON_INSTALL_BIN} COMPONENT treblotron)
endif()
