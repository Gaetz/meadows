#include "gameplay/stats/Damage.hpp"

#include <algorithm>

#include "gameplay/combat/Combat.hpp"    // updateLifeState
#include "gameplay/stats/CharacterStats.hpp" // recomputeStats

namespace gameplay {

const char* damageTypeName(DamageType type) {
    switch (type) {
    case DamageType::Slash:     return "Slash";
    case DamageType::Pierce:    return "Pierce";
    case DamageType::Blunt:     return "Blunt";
    case DamageType::Fire:      return "Fire";
    case DamageType::Cold:      return "Cold";
    case DamageType::Lightning: return "Lightning";
    case DamageType::Sonic:     return "Sonic";
    case DamageType::Chemical:  return "Chemical";
    case DamageType::Psychic:   return "Psychic";
    case DamageType::Holy:      return "Holy";
    case DamageType::Dark:      return "Dark";
    case DamageType::Ether:     return "Ether";
    }
    return "Unknown";
}

void killOutright(StatBlock& target, const GameplayTagRegistry& tags,
                  const DerivedStatRegistry& derived,
                  const StatsTuningForm& tuning) {
    // Every physical channel at once, at a magnitude no armor stack can
    // mitigate away — the normal pipeline does the rest (health to 0,
    // life state, the caller's OnDeath machinery).
    DamageEvent lethal;
    lethal.channels = { { DamageType::Slash, 1.0e7f },
                        { DamageType::Blunt, 1.0e7f },
                        { DamageType::Pierce, 1.0e7f } };
    applyDamage(target, lethal, tags, derived, nullptr, tuning);
}

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
    case DamageType::Fire:      field = "resistFire"; break;
    case DamageType::Cold:      field = "resistCold"; break;
    case DamageType::Lightning: field = "resistLightning"; break;
    case DamageType::Sonic:     field = "resistSonic"; break;
    case DamageType::Chemical:  field = "resistChemical"; break;
    case DamageType::Psychic:   field = "resistPsychic"; break;
    case DamageType::Holy:      field = "resistHoly"; break;
    case DamageType::Dark:      field = "resistDark"; break;
    case DamageType::Ether:     field = "resistEther"; break;
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
        // Negative resistance = vulnerability: it amplifies the hit (e.g. iron
        // armor conducts electricity). Floor at -100% so the worst case is ×2.
        f32 percent = std::clamp(mitigationPercent(sys, ch.type), -100.0f, 100.0f);
        // Chantier 6 C1: penetration eats POSITIVE protection only — it
        // never turns a vulnerability into a bigger one.
        const f32 pen =
            physical ? event.armorPenetration : event.resistPenetration;
        if (percent > 0.0f && pen > 0.0f) {
            percent = std::max(0.0f, percent - pen);
        }
        totalHealth += afterFlat * (1.0f - percent / 100.0f);
    }

    // Chantier 6 C1: the critical execution — criticalSensitivity% of the
    // target's max health × the attacker's multiplier, bypassing armor
    // entirely (docs/STATS.md §4). Fired by the attack site when the
    // target sits in its critical window.
    if (event.critical) {
        const f32 sensitivity = std::max(0.0f, cur("criticalSensitivity"));
        totalHealth += cur("maxHealth") * sensitivity / 100.0f *
                       event.criticalMultiplier;
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

    // Posture break → stagger + critical window (chantier 6 C2): posture
    // SITS at 0 for critWindowSeconds (updateCritWindow refills it on
    // expiry) and the target is open to the critical execution.
    if (target.combat.posture <= 0.0f && event.postureAmount > 0.0f) {
        result.staggered = true;
        target.combat.staggerSeconds = tuning.staggerSeconds;
        target.combat.critWindowSeconds = tuning.critWindowSeconds;
        if (const auto tag = tags.find("State.Staggered")) {
            target.system.tags.add(*tag, tags);
        }
        if (const auto tag = tags.find("State.CriticalWeakness")) {
            target.system.tags.add(*tag, tags);
        }
    } else if (event.postureAmount >
               (tuning.shakenThresholdBase + cur("constitution")) / 100.0f *
                   currentValueOf(target.system, attr("maxPosture"))) {
        // A heavy posture hit that does NOT break still rattles: brief
        // State.Shaken debuff (the break is strictly worse, so no stack).
        target.combat.shakenSeconds = tuning.shakenSeconds;
        if (const auto tag = tags.find("State.Shaken")) {
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

void updateCritWindow(CombatState& combat, AbilitySystem& system, f32 dt,
                      const GameplayTagRegistry& tags) {
    if (combat.critWindowSeconds <= 0.0f) {
        return;
    }
    combat.critWindowSeconds -= dt;
    if (combat.critWindowSeconds <= 0.0f) {
        combat.critWindowSeconds = 0.0f;
        combat.posture = currentValueOf(system, attr("maxPosture"));
        if (const auto tag = tags.find("State.CriticalWeakness")) {
            system.tags.remove(*tag, tags);
        }
    }
}

bool updateDowned(CombatState& combat, AbilitySystem& system, f32 dt,
                  const GameplayTagRegistry& tags) {
    // Only ticks while the actor is actually down — the tag is the state
    // (granted by updateLifeState under Follower.Protected), the timer is
    // only its bleedout clock.
    const auto downed = tags.find("State.Downed");
    if (!downed || !system.tags.has(*downed) || combat.downedSeconds <= 0.0f) {
        return false;
    }
    combat.downedSeconds -= dt;
    if (combat.downedSeconds <= 0.0f) {
        combat.downedSeconds = 0.0f;
        return true; // bleedout due — the caller rolls the outcome
    }
    return false;
}

void updateShaken(CombatState& combat, AbilitySystem& system, f32 dt,
                  const GameplayTagRegistry& tags) {
    if (combat.shakenSeconds <= 0.0f) {
        return;
    }
    combat.shakenSeconds -= dt;
    if (combat.shakenSeconds <= 0.0f) {
        combat.shakenSeconds = 0.0f;
        if (const auto tag = tags.find("State.Shaken")) {
            system.tags.remove(*tag, tags);
        }
    }
}

} // namespace gameplay
