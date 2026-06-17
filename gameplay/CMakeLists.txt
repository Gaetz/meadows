# meadows-gameplay — the simplified GAS (§6) and the rest of the 2D gameplay
# systems: attributes, gameplay effects, abilities, tags, the AbilitySystem
# component, factions. Depends on the data model and the ECS; render-free (§4).
# The GAS public surface is kept deliberately tiny.
add_library(meadows-gameplay STATIC
    ability/GameplayTags.cpp
    ability/AbilitySystem.cpp
    ability/GameplayEffects.cpp
    ability/GameplayAbility.cpp
    combat/Combat.cpp
    stats/CharacterStats.cpp
    stats/StatsTuning.cpp
    stats/Resonance.cpp
    stats/Survival.cpp
    stats/Damage.cpp
    stats/EquipmentStats.cpp
    stats/Rest.cpp
    stats/StatusBuildup.cpp
    stats/Injuries.cpp
    stats/Afflictions.cpp
    stats/Drugs.cpp
    stats/GameTime.cpp
    inventory/Inventory.cpp
    faction/Factions.cpp
    event/EventBus.cpp
    condition/Condition.cpp
)

target_link_libraries(meadows-gameplay PUBLIC meadows-data meadows-ecs)

meadows_set_warnings(meadows-gameplay)
