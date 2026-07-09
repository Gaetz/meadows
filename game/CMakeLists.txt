# meadows-runtime — the reusable runtime bridge: the single ECS↔rhi seam (scene
# submission, texture resolution). It sits ABOVE meadows and meadows-world, so
# the engine lib never depends on world/ (DAG stays acyclic). A future editor
# (in-engine or standalone) reuses this without the game executable.
add_library(meadows-runtime STATIC
    Barter.cpp
    FrameComposer.cpp
    InventoryView.cpp
    LevelEditor.cpp
    MeshCache.cpp
    SaveGame.cpp
    SceneStack.cpp
    SceneSubmit.cpp
    ScreenStack.cpp
    TerrainCollision.cpp
    TextureCache.cpp
    VegetationCollision.cpp
    WorldEditor.cpp
)
target_link_libraries(meadows-runtime PUBLIC meadows-render meadows-world
                      meadows-physics)
meadows_set_warnings(meadows-runtime)

add_executable(true-adventurer
    main.cpp
    AllForms.cpp
    scenes/WorldDemoScene.cpp
    scenes/DemoScenes.cpp
    scenes/CombatArenaScene.cpp
    scenes/EditorScene.cpp
    scenes/GameHud.cpp
    scenes/InteractionController.cpp
    scenes/LandscapeRenderer.cpp
    scenes/LandscapeScene.cpp
    scenes/NpcDirector.cpp
    scenes/PlayerController.cpp
    scenes/QuestDirector.cpp
    scenes/SaveController.cpp
    scenes/SceneConsole.cpp
    scenes/SceneEditor.cpp
    scenes/StreamingController.cpp
    scenes/TerrainSculptTool.cpp
    scenes/UiRouter.cpp
    scenes/WeatherController.cpp
    scenes/UiDemoScene.cpp
    ui/AnimGraphPanel.cpp
    ui/CharacterStatsPanel.cpp
    ui/ClipTimelinePanel.cpp
    ui/ConditionBuilder.cpp
    ui/ConsolePanel.cpp
    ui/DialogueGraphPanel.cpp
    ui/EventPicker.cpp
    ui/FormPicker.cpp
    ui/Keywords.cpp
    ui/NodeCanvas.cpp
    ui/PropertyGrid.cpp
    ui/QuestGraphPanel.cpp
)
target_link_libraries(true-adventurer PRIVATE meadows-runtime meadows-data
                      meadows-narrative meadows-script meadows-ui
                      meadows-audio imguizmo imgui-node-editor)
meadows_set_warnings(true-adventurer)

# Game data (plugins + assets) and engine shaders ship next to the executable.
add_custom_command(TARGET true-adventurer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/data
            $<TARGET_FILE_DIR:true-adventurer>/data
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${PROJECT_SOURCE_DIR}/engine/render/landscape/shaders
            $<TARGET_FILE_DIR:true-adventurer>/data/shaders
)
