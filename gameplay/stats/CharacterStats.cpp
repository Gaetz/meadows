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

void registerCoreDerivedStats(DerivedStatRegistry& registry,
                              const StatsTuningForm& tuning) {
    const reflect::TypeInfo* core = &CoreAttributes::staticTypeInfo();
    const StatsTuningForm t = tuning; // captured by each calculator (§5)

    // Primary maxima = Σ(linked attributes) × t.attributeToMax (docs/STATS.md §1).
    // They read the attributes' BASE (starting/leveled) value, NOT the current one
    // — a temporary attribute change (Resonance, buffs) does not move the max; only
    // Resonance's % does (§2). The Resonance offset still flows into the secondary
    // stats below, which read the CURRENT value (so Resonance weakens them).
    registry.add({ attr("maxHealth"), core, [t](const StatView& v) {
        return (v.base("strength") + v.base("constitution") + v.base("grace")) *
               t.attributeToMax;
    } });
    registry.add({ attr("maxEnergy"), core, [t](const StatView& v) {
        return (v.base("dexterity") + v.base("alacrity") + v.base("perception")) *
               t.attributeToMax;
    } });
    registry.add({ attr("maxEssence"), core, [t](const StatView& v) {
        return (v.base("charisma") + v.base("ego") + v.base("insight")) *
               t.attributeToMax;
    } });

    // Defensive stats (docs/STATS.md §3).
    const f32 m = t.mitigationPerAttribute;
    registry.add({ attr("defense"), core,
                   [m](const StatView& v) { return m * v.get("constitution"); } });
    registry.add({ attr("armorSlash"), core,
                   [m](const StatView& v) { return m * v.get("strength"); } });
    registry.add({ attr("armorBlunt"), core,
                   [m](const StatView& v) { return m * v.get("constitution"); } });
    registry.add({ attr("armorPierce"), core,
                   [m](const StatView& v) { return m * v.get("grace"); } });
    registry.add({ attr("resistFire"), core,
                   [m](const StatView& v) { return m * v.get("charisma"); } });
    registry.add({ attr("resistLightning"), core,
                   [m](const StatView& v) { return m * v.get("insight"); } });
    registry.add({ attr("will"), core, [t](const StatView& v) {
        return t.willBase + v.get("ego") * t.willPerEgo;
    } });
    registry.add({ attr("maxPosture"), core, [t](const StatView& v) {
        return t.basePosture + v.base("alacrity") * t.posturePerAlacrity;
    } });
    registry.add({ attr("postureRegen"), core, [t](const StatView& v) {
        return t.postureRegenBase + v.get("alacrity") * t.postureRegenPerAlacrity;
    } });
    registry.add({ attr("criticalSensitivity"), core, [t](const StatView& v) {
        return t.critSensBase - v.get("constitution") * t.critSensPerConstitution;
    } });
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
