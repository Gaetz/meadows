#include "gameplay/stats/CharacterStats.hpp"

#include "engine/ecs/World.hpp"
#include "gameplay/stats/CoreAttributes.hpp"

namespace gameplay {

void registerStatsComponents(ecs::World& world) {
    world.registerComponent<CoreAttributes>(); // reflected: base values serialize
}

namespace {
// Primary maxima = sum of the three linked attributes × 5 (docs/STATS.md §1):
// a starting humanoid (6/6/6, insight 0) has 90 / 90 / 60.
f32 maxHealthFormula(const StatView& v) {
    return (v.get("strength") + v.get("constitution") + v.get("grace")) * 5.0f;
}
f32 maxEnergyFormula(const StatView& v) {
    return (v.get("dexterity") + v.get("alacrity") + v.get("perception")) * 5.0f;
}
f32 maxEssenceFormula(const StatView& v) {
    return (v.get("charisma") + v.get("ego") + v.get("insight")) * 5.0f;
}
} // namespace

void registerCoreDerivedStats(DerivedStatRegistry& registry) {
    const reflect::TypeInfo* core = &CoreAttributes::staticTypeInfo();
    registry.add({ attr("maxHealth"), core, &maxHealthFormula });
    registry.add({ attr("maxEnergy"), core, &maxEnergyFormula });
    registry.add({ attr("maxEssence"), core, &maxEssenceFormula });
}

} // namespace gameplay
