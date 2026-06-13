# meadows-runtime — the reusable runtime bridge: the single ECS↔rhi seam (scene
# submission, texture resolution). It sits ABOVE meadows and meadows-world, so
# the engine lib never depends on world/ (DAG stays acyclic). A future editor
# (in-engine or standalone) reuses this without the game executable.
add_library(meadows-runtime STATIC
    SceneSubmit.cpp
    TextureCache.cpp
)
target_link_libraries(meadows-runtime PUBLIC meadows meadows-world)
meadows_set_warnings(meadows-runtime)

add_executable(true-adventurer main.cpp)
target_link_libraries(true-adventurer PRIVATE meadows-runtime meadows-data)
meadows_set_warnings(true-adventurer)

# Game data (plugins + assets) ships next to the executable.
add_custom_command(TARGET true-adventurer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/data
            $<TARGET_FILE_DIR:true-adventurer>/data
)
