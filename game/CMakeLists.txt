# meadows-runtime — the reusable runtime bridge: the single ECS↔rhi seam (scene
# submission, texture resolution). It sits ABOVE meadows and meadows-world, so
# the engine lib never depends on world/ (DAG stays acyclic). A future editor
# (in-engine or standalone) reuses this without the game executable.
add_library(meadows-runtime STATIC
    Barter.cpp
    InputActions.cpp
    InventoryView.cpp
    LevelEditor.cpp
    MapRaster.cpp
    SaveGame.cpp
    SceneStack.cpp
    SceneSubmit.cpp
    ScreenStack.cpp
    Settings.cpp
    SoundResolver.cpp
    TerrainBakeStreamer.cpp
    TerrainCollision.cpp
    scenes/DungeonGenTool.cpp
    scenes/TerrainGenTool.cpp
    RendererAssets.cpp
    VegetationCollision.cpp
    WeaponMeshes.cpp
)
target_link_libraries(meadows-runtime PUBLIC meadows-render meadows-world
                      meadows-physics meadows-audio
                      PRIVATE tomlplusplus::tomlplusplus) # settings.toml (C9.2)
# Same no-exceptions toml++ mode as meadows-data (parse_result error path).
target_compile_definitions(meadows-runtime PRIVATE TOML_EXCEPTIONS=0)
meadows_set_warnings(meadows-runtime)

add_executable(true-adventurer
    main.cpp
    AllForms.cpp
    scenes/WorldDemoScene.cpp
    scenes/CombatArenaScene.cpp
    scenes/EditorScene.cpp
    scenes/FollowerController.cpp
    scenes/FxDirector.cpp
    scenes/GameHud.cpp
    scenes/InteractionController.cpp
    scenes/LandscapeScene.cpp
    scenes/MapController.cpp
    scenes/NpcCombatController.cpp
    scenes/NpcDirector.cpp
    scenes/NpcMovement.cpp
    scenes/NpcScheduleController.cpp
    scenes/NpcSpawner.cpp
    scenes/OptionsController.cpp
    scenes/PlayerController.cpp
    scenes/ProjectileDirector.cpp
    scenes/QuestDirector.cpp
    scenes/RenderTuningIo.cpp
    scenes/RideController.cpp
    scenes/SaveController.cpp
    scenes/SceneConsole.cpp
    scenes/SceneEditor.cpp
    scenes/StreamingController.cpp
    scenes/TerrainSculptTool.cpp
    scenes/TreeCreationScene.cpp
    scenes/UiRouter.cpp
    scenes/WeatherController.cpp
    ui/AbilityPanel.cpp
    ui/AnimGraphPanel.cpp
    ui/AnimPreviewPanel.cpp
    ui/ClipTimelinePanel.cpp
    ui/ConditionBuilder.cpp
    ui/ConsolePanel.cpp
    ui/DialogueGraphPanel.cpp
    ui/EffectPanel.cpp
    ui/EventPicker.cpp
    ui/FieldWidgets.cpp
    ui/FormPicker.cpp
    ui/FxPanel.cpp
    ui/Keywords.cpp
    ui/NodeCanvas.cpp
    ui/PropertyGrid.cpp
    ui/QuestGraphPanel.cpp
    ui/RenderTuningPanels.cpp
)
target_link_libraries(true-adventurer PRIVATE meadows-runtime meadows-data
                      meadows-narrative meadows-script meadows-ui
                      meadows-audio imguizmo imgui-node-editor)
meadows_set_warnings(true-adventurer)

# Game data (plugins + assets) and engine shaders ship next to the
# executable — as a custom target the exe DEPENDS on, so it runs on EVERY
# build (a POST_BUILD only fired when the exe relinked: a data-only edit
# left the build dir stale — the 2026-07-12 lesson). `_if_different`
# (CMake 3.26) only rewrites what changed.
# Terrain material auto-cook: when the CC0 source maps are present
# (assets-src/ is gitignored — absent on a fresh clone, where the
# committed .mtex simply ship as-is), re-run the cooker whenever the
# manifest or any source PNG is newer than the cooked arrays. Runs
# BEFORE game-data so the fresh .mtex are what gets copied.
set(TERRAIN_MATERIALS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/data/base/textures/terrain)
set(TERRAIN_MATERIALS_MANIFEST
    ${CMAKE_CURRENT_SOURCE_DIR}/data/base/terrain/materials.toml)
if(EXISTS ${PROJECT_SOURCE_DIR}/assets-src/terrain)
    file(GLOB_RECURSE TERRAIN_MATERIAL_SOURCES
         ${PROJECT_SOURCE_DIR}/assets-src/terrain/*.png)
    add_custom_command(
        OUTPUT ${TERRAIN_MATERIALS_DIR}/terrain_albedo.mtex
               ${TERRAIN_MATERIALS_DIR}/terrain_normal.mtex
               ${TERRAIN_MATERIALS_DIR}/terrain_orm.mtex
               ${TERRAIN_MATERIALS_DIR}/terrain_height.mtex
        COMMAND $<TARGET_FILE:cooker> cook-terrain-materials
                ${TERRAIN_MATERIALS_MANIFEST} ${TERRAIN_MATERIALS_DIR}
        DEPENDS cooker ${TERRAIN_MATERIALS_MANIFEST}
                ${TERRAIN_MATERIAL_SOURCES}
        COMMENT "Cooking terrain material arrays (.mtex)"
        VERBATIM
    )
    add_custom_target(terrain-materials
        DEPENDS ${TERRAIN_MATERIALS_DIR}/terrain_albedo.mtex
                ${TERRAIN_MATERIALS_DIR}/terrain_normal.mtex
                ${TERRAIN_MATERIALS_DIR}/terrain_orm.mtex
                ${TERRAIN_MATERIALS_DIR}/terrain_height.mtex)
else()
    add_custom_target(terrain-materials)
endif()

add_custom_target(game-data
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/data
            $<TARGET_FILE_DIR:true-adventurer>/data
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
            ${PROJECT_SOURCE_DIR}/engine/render/landscape/shaders
            $<TARGET_FILE_DIR:true-adventurer>/data/shaders
    COMMENT "Syncing game data + shaders"
    VERBATIM
)
add_dependencies(game-data terrain-materials)
add_dependencies(true-adventurer game-data)
