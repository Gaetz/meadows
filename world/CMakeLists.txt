# meadows-world — the world layer: world data model (worldspace/cell/reference
# Forms), scene representation and the reference→entity spawner. Depends on the
# data model and (from brick b onward) the ECS. Never pulls in rhi/render (§4);
# the ECS↔rhi seam lives above, in the game/runtime layer.
add_library(meadows-world STATIC
    worldspace/WorldForms.cpp
    worldspace/FormCategory.cpp
    worldspace/WorldModel.cpp
    scene/AnimBridge.cpp
    scene/Components.cpp
    scene/Spawner.cpp
    scene/Movement.cpp
    scene/Collision.cpp
    scene/TriggerSystem.cpp
    scene/SpatialIndex.cpp
    ai/Pathfinding.cpp
    ai/Perception.cpp
    ai/AiController.cpp
    ai/GridNavigator.cpp
    ai/TerrainNavigator.cpp
    streaming/CellLoader.cpp
    streaming/CellStreamer.cpp
    terrain/TerrainPatches.cpp
)

# meadows-gameplay: the Actor spawner wires the mandatory GAS components (§2.7).
target_link_libraries(meadows-world PUBLIC meadows-data meadows-ecs meadows-gameplay)

meadows_set_warnings(meadows-world)
