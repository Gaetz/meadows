#include "gameplay/stats/CharacterStats.hpp"

#include "engine/ecs/World.hpp"
#include "gameplay/stats/CoreAttributes.hpp"
#include "gameplay/stats/Resonance.hpp"
#include "gameplay/stats/Survival.hpp"

namespace gameplay {

void registerStatsComponents(ecs::World& world) {
    world.registerComponent<CoreAttributes>(); // reflected: base values serialize
    world.registerComponent<Resonance>();      // reflected: hidden phase stat
    world.registerComponent<Survival>();       // reflected: hunger / thirst
}

namespace {
// Primary maxima = sum of the three linked attributes × 5 (docs/STATS.md §1):
// a starting humanoid (6/6/6, insight 0) has 90 / 90 / 60. They read the
// attributes' BASE (starting/leveled) value, NOT the current one — so a temporary
// attribute change (Resonance, buffs) does not move the max; only Resonance's %
// does (§2). The Resonance attribute offset still flows into the secondary stats,
// which read the current value.
f32 maxHealthFormula(const StatView& v) {
    return (v.base("strength") + v.base("constitution") + v.base("grace")) * 5.0f;
}
f32 maxEnergyFormula(const StatView& v) {
    return (v.base("dexterity") + v.base("alacrity") + v.base("perception")) * 5.0f;
}
f32 maxEssenceFormula(const StatView& v) {
    return (v.base("charisma") + v.base("ego") + v.base("insight")) * 5.0f;
}
} // namespace

void registerCoreDerivedStats(DerivedStatRegistry& registry) {
    const reflect::TypeInfo* core = &CoreAttributes::staticTypeInfo();
    registry.add({ attr("maxHealth"), core, &maxHealthFormula });
    registry.add({ attr("maxEnergy"), core, &maxEnergyFormula });
    registry.add({ attr("maxEssence"), core, &maxEssenceFormula });
}

} // namespace gameplay
