# meadows-gameplay — the simplified GAS (§6) and the rest of the 2D gameplay
# systems: attributes, gameplay effects, abilities, tags, the AbilitySystem
# component, factions. Depends on the data model and the ECS; render-free (§4).
# The GAS public surface is kept deliberately tiny.
add_library(meadows-gameplay STATIC
    ability/GameplayTags.cpp
)

target_link_libraries(meadows-gameplay PUBLIC meadows-data meadows-ecs)

meadows_set_warnings(meadows-gameplay)
