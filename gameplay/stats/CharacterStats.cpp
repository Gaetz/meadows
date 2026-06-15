#include "gameplay/stats/CharacterStats.hpp"

#include "engine/ecs/World.hpp"
#include "gameplay/ability/GameplayEffects.hpp" // recomputeCurrent
#include "gameplay/stats/Damage.hpp"            // CombatState
#include "gameplay/stats/Resonance.hpp"
#include "gameplay/stats/Survival.hpp"

namespace gameplay {

void registerStatsComponents(ecs::World& world) {
    world.registerComponent<CoreAttributes>(); // reflected: base values serialize
    world.registerComponent<Resonance>();      // reflected: hidden phase stat
    world.registerComponent<Survival>();       // reflected: hunger / thirst
    world.handle().component<CombatState>();    // runtime: posture / stagger
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

// Defensive stats (docs/STATS.md §3). Mitigation/offense secondaries read the
// CURRENT attribute (Resonance weakens them); maxPosture is a "max" and reads the
// base (like the primary maxima). Caps/percentages are applied at use (Damage).
f32 defenseFormula(const StatView& v) { return 0.5f * v.get("constitution"); }
f32 armorSlashFormula(const StatView& v) { return 0.5f * v.get("strength"); }
f32 armorBluntFormula(const StatView& v) { return 0.5f * v.get("constitution"); }
f32 armorPierceFormula(const StatView& v) { return 0.5f * v.get("grace"); }
f32 resistFireFormula(const StatView& v) { return 0.5f * v.get("charisma"); }
f32 resistLightningFormula(const StatView& v) { return 0.5f * v.get("insight"); }
f32 willFormula(const StatView& v) { return 1.0f + v.get("ego") / 4.0f; }
f32 maxPostureFormula(const StatView& v) { return 50.0f + v.base("alacrity"); }
f32 postureRegenFormula(const StatView& v) { return 2.0f + v.get("alacrity") / 3.0f; }
f32 criticalSensitivityFormula(const StatView& v) {
    return 25.0f - v.get("constitution") * 0.1f;
}
} // namespace

void registerCoreDerivedStats(DerivedStatRegistry& registry) {
    const reflect::TypeInfo* core = &CoreAttributes::staticTypeInfo();
    registry.add({ attr("maxHealth"), core, &maxHealthFormula });
    registry.add({ attr("maxEnergy"), core, &maxEnergyFormula });
    registry.add({ attr("maxEssence"), core, &maxEssenceFormula });
    registry.add({ attr("defense"), core, &defenseFormula });
    registry.add({ attr("armorSlash"), core, &armorSlashFormula });
    registry.add({ attr("armorBlunt"), core, &armorBluntFormula });
    registry.add({ attr("armorPierce"), core, &armorPierceFormula });
    registry.add({ attr("resistFire"), core, &resistFireFormula });
    registry.add({ attr("resistLightning"), core, &resistLightningFormula });
    registry.add({ attr("will"), core, &willFormula });
    registry.add({ attr("maxPosture"), core, &maxPostureFormula });
    registry.add({ attr("postureRegen"), core, &postureRegenFormula });
    registry.add({ attr("criticalSensitivity"), core, &criticalSensitivityFormula });
}

void recomputeStats(const CoreAttributes& core, const AttributeSet& vitals,
                    AbilitySystem& system, const DerivedStatRegistry& derived,
                    const StatModifiers* extra) {
    const AttrSetRef sets[] = {
        { &CoreAttributes::staticTypeInfo(), &core },
        { &AttributeSet::staticTypeInfo(), &vitals },
    };
    recomputeCurrent(system, sets, &derived, extra);
}

} // namespace gameplay
