#include "gameplay/stats/Damage.hpp"

#include <algorithm>

#include "gameplay/combat/Combat.hpp"    // updateLifeState
#include "gameplay/stats/CharacterStats.hpp" // recomputeStats

namespace gameplay {

namespace {
bool isPhysical(DamageType type) {
    return type == DamageType::Slash || type == DamageType::Pierce ||
           type == DamageType::Blunt;
}

// The percentage mitigation (armor for physical, resistance for elemental) for a
// channel, read from the recomputed overlay.
f32 mitigationPercent(const AbilitySystem& system, DamageType type) {
    const char* field = "";
    switch (type) {
    case DamageType::Slash:     field = "armorSlash"; break;
    case DamageType::Pierce:    field = "armorPierce"; break;
    case DamageType::Blunt:     field = "armorBlunt"; break;
    case DamageType::Cold:      field = "resistCold"; break;
    case DamageType::Fire:      field = "resistFire"; break;
    case DamageType::Lightning: field = "resistLightning"; break;
    }
    return currentValueOf(system, attr(field));
}
} // namespace

DamageResult applyDamage(StatBlock& target, const DamageEvent& event,
                         const GameplayTagRegistry& tags,
                         const DerivedStatRegistry& derived,
                         const StatModifiers* extra, const StatsTuningForm& tuning) {
    const AbilitySystem& sys = target.system;
    const auto cur = [&](const char* name) { return currentValueOf(sys, attr(name)); };

    // Per channel: flat reduction (defense / will, capped at 25 + attr % of the
    // raw amount) then percentage reduction (armor / resistance, 0..100%).
    f32 totalHealth = 0.0f;
    for (const DamageChannel& ch : event.channels) {
        const bool physical = isPhysical(ch.type);
        const f32 flatStat = physical ? cur("defense") : cur("will");
        const f32 capAttr = physical ? cur("constitution") : cur("ego");
        const f32 flatCap =
            (tuning.flatMitigationCapBase + capAttr) / 100.0f * ch.amount;
        const f32 flat = std::min(flatStat, flatCap);
        const f32 afterFlat = std::max(0.0f, ch.amount - flat);
        const f32 percent = std::clamp(mitigationPercent(sys, ch.type), 0.0f, 100.0f);
        totalHealth += afterFlat * (1.0f - percent / 100.0f);
    }

    // Deplete health (the resource this execution calculation drains, §6/§2.9).
    const f32 healthBase = baseValueOf(target.vitals, attr("health")).value_or(0.0f);
    setBaseValue(target.vitals, attr("health"), std::max(0.0f, healthBase - totalHealth));

    // Deplete posture (combat resource).
    target.combat.posture = std::max(0.0f, target.combat.posture - event.postureAmount);

    // A hit interrupts Rest (§5): injury/resonance recovery restarts after sleep.
    if (totalHealth > 0.0f || event.postureAmount > 0.0f) {
        target.combat.restSeconds = 0.0f;
    }

    recomputeStats(target.core, target.vitals, target.system, derived, extra);

    DamageResult result;
    result.healthDamage = totalHealth;
    result.postureDamage = event.postureAmount;

    // Posture break → stagger: grant the tag, start the timer, restore posture.
    if (target.combat.posture <= 0.0f && event.postureAmount > 0.0f) {
        result.staggered = true;
        target.combat.staggerSeconds = tuning.staggerSeconds;
        target.combat.posture = currentValueOf(target.system, attr("maxPosture"));
        if (const auto tag = tags.find("State.Staggered")) {
            target.system.tags.add(*tag, tags);
        }
    }

    updateLifeState(target.system, tags); // health 0 → State.Dead
    return result;
}

void updateStagger(CombatState& combat, AbilitySystem& system, f32 dt,
                   const GameplayTagRegistry& tags) {
    if (combat.staggerSeconds <= 0.0f) {
        return;
    }
    combat.staggerSeconds -= dt;
    if (combat.staggerSeconds <= 0.0f) {
        combat.staggerSeconds = 0.0f;
        if (const auto tag = tags.find("State.Staggered")) {
            system.tags.remove(*tag, tags);
        }
    }
}

void updateParalysis(CombatState& combat, AbilitySystem& system, f32 dt,
                     const GameplayTagRegistry& tags) {
    if (combat.paralysisSeconds <= 0.0f) {
        return;
    }
    combat.paralysisSeconds -= dt;
    if (combat.paralysisSeconds <= 0.0f) {
        combat.paralysisSeconds = 0.0f;
        if (const auto tag = tags.find("State.Paralyzed")) {
            system.tags.remove(*tag, tags);
        }
    }
}

} // namespace gameplay
