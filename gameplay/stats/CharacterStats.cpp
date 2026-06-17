#include "gameplay/stats/CharacterStats.hpp"

#include "engine/ecs/World.hpp"
#include "gameplay/ability/GameplayEffects.hpp" // recomputeCurrent
#include "gameplay/stats/Afflictions.hpp"       // Afflictions
#include "gameplay/stats/Damage.hpp"            // CombatState
#include "gameplay/stats/Drugs.hpp"             // ActiveDrugs
#include "gameplay/stats/Injuries.hpp"          // Injuries
#include "gameplay/stats/Resonance.hpp"
#include "gameplay/stats/StatusBuildup.hpp"
#include "gameplay/stats/Survival.hpp"

namespace gameplay {

void registerStatsComponents(ecs::World& world) {
    world.registerComponent<CoreAttributes>();  // reflected: base values serialize
    world.registerComponent<Resonance>();       // reflected: hidden phase stat
    world.registerComponent<Survival>();        // reflected: hunger / thirst
    world.registerComponent<StatusBuildup>();   // reflected: status accumulators
    world.handle().component<CombatState>();     // runtime: posture / stagger
    world.handle().component<Injuries>();        // runtime: body-part injuries
    world.handle().component<Afflictions>();     // runtime: diseases / psychoses
    world.handle().component<ActiveDrugs>();     // runtime: active drugs (N4)
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
    registry.add({ attr("resistCold"), core,
                   [m](const StatView& v) { return m * v.get("ego"); } });
    registry.add({ attr("resistLightning"), core,
                   [m](const StatView& v) { return m * v.get("insight"); } });
    registry.add({ attr("will"), core, [t](const StatView& v) {
        return t.willBase + v.get("ego") * t.willPerEgo;
    } });
    // Vitality: reduces a status's ongoing damage by % (docs/STATS.md §3).
    // Formula: min(1 + alacrity/4, 25 + alacrity) expressed as a percentage.
    registry.add({ attr("vitality"), core, [](const StatView& v) {
        const f32 a = v.get("alacrity");
        return std::min(1.0f + a / 4.0f, 25.0f + a);
    } });
    registry.add({ attr("maxPosture"), core, [t](const StatView& v) {
        return t.basePosture + v.base("alacrity") * t.posturePerAlacrity;
    } });
    registry.add({ attr("postureRegen"), core, [t](const StatView& v) {
        return t.postureRegenBase + v.get("alacrity") * t.postureRegenPerAlacrity;
    } });
    registry.add({ attr("energyRegen"), core, [t](const StatView& v) {
        return t.energyRegenBase + v.get("alacrity") * t.energyRegenPerAlacrity;
    } });
    registry.add({ attr("healthRegen"), core, [t](const StatView& v) {
        return v.get("grace") * t.healthRegenPerGrace;
    } });
    registry.add({ attr("essenceRegen"), core, [t](const StatView& v) {
        return t.essenceRegenBase + v.get("insight") * t.essenceRegenPerInsight;
    } });
    registry.add({ attr("criticalSensitivity"), core, [t](const StatView& v) {
        return t.critSensBase - v.get("constitution") * t.critSensPerConstitution;
    } });

    // Endurance = the per-type status-buildup threshold (docs/STATS.md §3, N1):
    // dexterity → poison/bleed, alacrity → mental/disease, perception → curse/death.
    const auto endurance = [&](const char* stat, const char* attribute) {
        registry.add({ attr(stat), core, [t, attribute](const StatView& v) {
            return t.enduranceBase + v.get(attribute) * t.endurancePerAttribute;
        } });
    };
    endurance("endurancePoison", "dexterity");
    endurance("enduranceBleed", "dexterity");
    endurance("enduranceMental", "alacrity");
    endurance("enduranceDisease", "alacrity");
    endurance("enduranceCurse", "perception");
    endurance("enduranceDeath", "perception");
    // Elemental endurance = enduranceBase + matching resistance (docs/STATS.md §3).
    // resistFire = m*charisma, resistCold = m*ego, resistLightning = m*insight.
    registry.add({ attr("enduranceIgnition"), core, [t, m](const StatView& v) {
        return t.enduranceBase + m * v.get("charisma");
    } });
    registry.add({ attr("enduranceGlaciation"), core, [t, m](const StatView& v) {
        return t.enduranceBase + m * v.get("ego");
    } });
    registry.add({ attr("enduranceElectrocution"), core, [t, m](const StatView& v) {
        return t.enduranceBase + m * v.get("insight");
    } });

    // Movement speed (docs/STATS.md §3) — minimal, so leg-injury speed maluses
    // (N2) have a target; full utility/encumbrance is a later pass.
    registry.add({ attr("movementSpeed"), core, [](const StatView& v) {
        return 90.0f + v.get("alacrity") + v.get("strength");
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
