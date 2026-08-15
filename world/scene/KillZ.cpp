#include "world/scene/KillZ.hpp"

#include "engine/core/Log.hpp"
#include "gameplay/stats/Damage.hpp" // StatBlock, killOutright
#include "world/scene/Components.hpp"

namespace world {

void enforceKillZ(ecs::World& world, f32 killZ,
                  const gameplay::GameplayTagRegistry& tags,
                  const gameplay::DerivedStatRegistry& derived,
                  const gameplay::StatsTuningForm& tuning,
                  ecs::Entity exempt) {
    const auto dead = tags.find("State.Dead");

    // Snapshot DURING the query, act AFTER it (the TriggerSystem
    // discipline): killOutright only mutates existing components today,
    // but the house pattern stays immune to future drift.
    vector<ecs::Entity> below;
    world.handle()
        .query_builder<const Transform, const gameplay::AbilitySystem>()
        .build()
        .each([&](flecs::entity entity, const Transform& transform,
                  const gameplay::AbilitySystem& system) {
            if (transform.position.y >= killZ || entity == exempt) {
                return;
            }
            if (dead && system.tags.has(*dead)) {
                return; // already a corpse — no re-kill, no log spam
            }
            below.push_back(entity);
        });

    for (ecs::Entity entity : below) {
        if (!entity.is_alive() || !entity.has<gameplay::AbilitySystem>()) {
            continue;
        }
        gameplay::StatBlock block {
            entity.get_mut<gameplay::CoreAttributes>(),
            entity.get_mut<gameplay::AttributeSet>(),
            entity.get_mut<gameplay::AbilitySystem>(),
            entity.get_mut<gameplay::CombatState>()
        };
        gameplay::killOutright(block, tags, derived, tuning);
        LOG_INFO("an actor fell below killZ ({:.0f})", killZ);
    }
}

} // namespace world
