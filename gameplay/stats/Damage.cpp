#include "gameplay/stats/Damage.hpp"

#include <algorithm>

#include "gameplay/combat/Combat.hpp"    // updateLifeState
#include "gameplay/stats/CharacterStats.hpp" // recomputeStats

namespace gameplay {

const char* damageTypeName(DamageType type) {
    if (static_cast<size_t>(type) >= std::size(kDamageTypes)) {
        return "Unknown";
    }
    return damageTraits(type).name;
}

DamageType dominantDamageType(const DamageEvent& event) {
    f32 summed[std::size(kDamageTypes)] = {};
    for (const DamageChannel& channel : event.channels) {
        summed[static_cast<size_t>(channel.type)] += channel.amount;
    }
    DamageType dominant = DamageType::Slash;
    f32 best = 0.0f;
    for (size_t i = 0; i < std::size(kDamageTypes); ++i) {
        if (summed[i] > best) { // strict: the first type keeps a tie
            best = summed[i];
            dominant = static_cast<DamageType>(i);
        }
    }
    return dominant;
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

f32 fallDamage(f32 fallHeight, const StatsTuningForm& tuning) {
    if (fallHeight < tuning.fallMinHeight) {
        return 0.0f;
    }
    return (fallHeight - tuning.fallMinHeight) * tuning.fallDamagePerMeter;
}

namespace {
// The percentage mitigation (armor for physical, resistance for elemental) for a
// channel, read from the recomputed overlay.
f32 mitigationPercent(const AbilitySystem& system, DamageType type) {
    return currentValueOf(system, attr(damageTraits(type).mitigationAttr));
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
        const bool physical = damageTraits(ch.type).physical;
        const f32 flatStat = physical ? cur("defense") : cur("will");
        const f32 capAttr = physical ? cur("constitution") : cur("ego");
        const f32 flatCap =
            (tuning.flatMitigationCapBase + capAttr) / 100.0f * ch.amount;
        const f32 flat = std::min(flatStat, flatCap);
        const f32 afterFlat = std::max(0.0f, ch.amount - flat);
        // Negative resistance = vulnerability: it amplifies the hit (e.g. iron
        // armor conducts electricity). Floor at -100% so the worst case is ×2.
        f32 percent = std::clamp(mitigationPercent(sys, ch.type), -100.0f, 100.0f);
        // Penetration eats POSITIVE protection only — it
        // never turns a vulnerability into a bigger one.
        const f32 pen =
            physical ? event.armorPenetration : event.resistPenetration;
        if (percent > 0.0f && pen > 0.0f) {
            percent = std::max(0.0f, percent - pen);
        }
        totalHealth += afterFlat * (1.0f - percent / 100.0f);
    }

    // The critical execution — criticalSensitivity% of the
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

    // Posture break → stagger + critical window (docs/STATS.md §4): posture
    // SITS at 0 for critWindowSeconds (updateCritWindow refills it on
    // expiry) and the target is open to the critical execution.
    if (target.combat.posture <= 0.0f && event.postureAmount > 0.0f) {
        result.staggered = true;
        grantTimedState(target.combat, target.system, tags, TimedState::Staggered,
                        tuning.staggerSeconds, Grant::Overwrite);
        grantTimedState(target.combat, target.system, tags, TimedState::CriticalWindow,
                        tuning.critWindowSeconds, Grant::Overwrite);
    } else if (event.postureAmount >
               (tuning.shakenThresholdBase + cur("constitution")) / 100.0f *
                   currentValueOf(target.system, attr("maxPosture"))) {
        // A heavy posture hit that does NOT break still rattles: brief
        // State.Shaken debuff (the break is strictly worse, so no stack).
        grantTimedState(target.combat, target.system, tags, TimedState::Shaken,
                        tuning.shakenSeconds, Grant::Overwrite);
    }

    updateLifeState(target.system, tags); // health 0 → State.Dead
    return result;
}

void grantTimedState(CombatState& combat, AbilitySystem& system,
                     const GameplayTagRegistry& tags, TimedState state,
                     f32 seconds, Grant grant) {
    const TimedStateTraits& row = kTimedStates[static_cast<size_t>(state)];
    f32& timer = combat.*row.timer;
    timer = grant == Grant::Extend ? std::max(timer, seconds) : seconds;
    if (const auto tag = tags.find(row.tag)) {
        system.tags.add(*tag, tags);
    }
}

namespace {
void tickTimedState(CombatState& combat, AbilitySystem& system, f32 dt,
                    const GameplayTagRegistry& tags, const TimedStateTraits& row) {
    f32& timer = combat.*row.timer;
    if (timer <= 0.0f) {
        return;
    }
    timer -= dt;
    if (timer <= 0.0f) {
        timer = 0.0f;
        if (row.onExpire == OnExpire::RefillPosture) {
            combat.posture = currentValueOf(system, attr("maxPosture"));
        }
        if (const auto tag = tags.find(row.tag)) {
            system.tags.remove(*tag, tags);
        }
    }
}
} // namespace

void tickTimedStates(CombatState& combat, AbilitySystem& system, f32 dt,
                     const GameplayTagRegistry& tags) {
    for (const TimedStateTraits& row : kTimedStates) {
        tickTimedState(combat, system, dt, tags, row);
    }
}

void updateStagger(CombatState& combat, AbilitySystem& system, f32 dt,
                   const GameplayTagRegistry& tags) {
    tickTimedState(combat, system, dt, tags,
                   kTimedStates[static_cast<size_t>(TimedState::Staggered)]);
}

void updateParalysis(CombatState& combat, AbilitySystem& system, f32 dt,
                     const GameplayTagRegistry& tags) {
    tickTimedState(combat, system, dt, tags,
                   kTimedStates[static_cast<size_t>(TimedState::Paralyzed)]);
}

void updateCritWindow(CombatState& combat, AbilitySystem& system, f32 dt,
                      const GameplayTagRegistry& tags) {
    tickTimedState(combat, system, dt, tags,
                   kTimedStates[static_cast<size_t>(TimedState::CriticalWindow)]);
}

void updateShaken(CombatState& combat, AbilitySystem& system, f32 dt,
                  const GameplayTagRegistry& tags) {
    tickTimedState(combat, system, dt, tags,
                   kTimedStates[static_cast<size_t>(TimedState::Shaken)]);
}

bool updateDowned(CombatState& combat, AbilitySystem& system, f32 dt,
                  const GameplayTagRegistry& tags) {
    // Only ticks while the actor is actually down — the tag is the state
    // (granted by updateLifeState under Follower.Protected), the timer is
    // only its bleedout clock. Not a kTimedStates row: gated by the tag and
    // the CALLER resolves the expiry.
    const auto downed = tags.find(tags::Downed);
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

} // namespace gameplay
