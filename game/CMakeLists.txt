add_executable(true-adventurer main.cpp)
target_link_libraries(true-adventurer PRIVATE meadows meadows-data)
meadows_set_warnings(true-adventurer)

# Game data (plugins + assets) ships next to the executable.
add_custom_command(TARGET true-adventurer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/data
            $<TARGET_FILE_DIR:true-adventurer>/data
)
