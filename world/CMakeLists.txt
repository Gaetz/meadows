# meadows-world — the world layer: world data model (worldspace/cell/reference
# Forms), scene representation and the reference→entity spawner. Depends on the
# data model and (from brick b onward) the ECS. Never pulls in rhi/render (§4);
# the ECS↔rhi seam lives above, in the game/runtime layer.
add_library(meadows-world STATIC
    worldspace/WorldForms.cpp
    worldspace/FormCategory.cpp
    worldspace/WorldModel.cpp
    scene/Components.cpp
    scene/Spawner.cpp
    streaming/CellLoader.cpp
)

target_link_libraries(meadows-world PUBLIC meadows-data meadows-ecs)

meadows_set_warnings(meadows-world)
