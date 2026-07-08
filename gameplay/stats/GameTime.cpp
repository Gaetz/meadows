#include "gameplay/stats/GameTime.hpp"

#include <algorithm>

#include "gameplay/ability/GameplayEffects.hpp" // tickGameTimeEffects
#include "gameplay/combat/Combat.hpp"            // updateLifeState
#include "gameplay/stats/Rest.hpp"               // accrueRest
#include "gameplay/stats/ResonanceDecays.hpp"    // tickResonanceDecays

namespace gameplay {

namespace {

bool isDead(const GameTimeTickArgs& a) {
    const auto dead = a.tags.find("State.Dead");
    return dead && a.system.tags.has(*dead);
}

bool applyBuildupResult(GameTimeTickArgs& a, const BuildupTickResult& br,
                        const DerivedStatRegistry& derived, const StatModifiers& mods) {
    // §2.9 execution calc: buildup DoT / lethal zeroing drain BaseValues
    // directly (final per-tick amounts — see CharacterTick). updateLifeState
    // below turns health 0 into State.Dead.
    if (br.poisonHealthDamage > 0.0f || br.ignitionHealthDamage > 0.0f) {
        a.vitals.health = std::max(0.0f,
            a.vitals.health - br.poisonHealthDamage - br.ignitionHealthDamage);
    }
    if (br.electrocutionEssenceDamage > 0.0f) {
        a.vitals.essence = std::max(0.0f,
            a.vitals.essence - br.electrocutionEssenceDamage);
    }
    if (br.bleedBurst) {
        StatBlock block { a.core, a.vitals, a.system, a.combat };
        applyDamage(block,
            DamageEvent { { { DamageType::Slash, a.tuning.bleedBurstDamage } }, 0.0f },
            a.tags, derived, &mods, a.tuning);
    }
    if (br.electrocutionTriggered) {
        const f32 maxP = currentValueOf(a.system, attr("maxPosture"));
        a.combat.posture = std::max(0.0f,
            a.combat.posture - maxP * a.tuning.electrocutionPostureDrainPercent);
    }
    if (br.glaciationTriggered) {
        a.combat.paralysisSeconds = std::max(
            a.combat.paralysisSeconds, a.tuning.glaciationParalysisDuration);
        if (const auto tag = a.tags.find("State.Paralyzed")) {
            a.system.tags.add(*tag, a.tags);
        }
    }
    if (br.deathTriggered) {
        a.vitals.health = 0.0f;
    }
    if (a.vitals.health <= 0.0f) {
        updateLifeState(a.system, a.tags);
        return true;
    }
    return false;
}

} // namespace

StatModifiers buildCharacterMods(GameTimeTickArgs& a, const StatModifiers& equipmentMods) {
    // All resonance sources (survival, injuries, afflictions, drugs) are now GAS
    // activeEffects — Phase A recompute already included them in currentValueOf().
    Resonance eff;
    eff.onyx   = currentValueOf(a.system, attr("onyx"));
    eff.amber  = currentValueOf(a.system, attr("amber"));
    eff.garnet = currentValueOf(a.system, attr("garnet"));

    // Decaying contributions from expired resonance effects.
    addResonanceDecayToResonance(a.resoDecays, eff);

    StatModifiers mods;
    buildResonanceModifiers(eff, mods, harmonyBroken(a.system, a.tags));

    // Merge equipment / fixed mods.
    for (const auto& [k, v] : equipmentMods.add) { mods.add[k] += v; }
    for (const auto& [k, v] : equipmentMods.mul) {
        auto [it, inserted] = mods.mul.try_emplace(k, 1.0f);
        it->second *= v;
    }

    buildupStatusModifiers(a.system, a.tags, a.tuning, mods);
    return mods;
}

void tickGameTime(GameTimeTickArgs& a, f64 gameDt, const StatModifiers& mods) {
    const auto cur = [&](const char* n) { return currentValueOf(a.system, attr(n)); };
    const f32 gdt = static_cast<f32>(gameDt);

    // §2.9 execution calc: rate-driven regen (see CharacterTick) — game-time path.
    // Health and essence regen (game-time; very slow — docs/STATS.md §3).
    a.vitals.health  = std::min(cur("maxHealth"),  a.vitals.health  + cur("healthRegen")  * gdt);
    a.vitals.essence = std::min(cur("maxEssence"), a.vitals.essence + cur("essenceRegen") * gdt);

    // Survival needs decay (game-time).
    tickSurvival(a.survival, gameDt, a.tuning);

    // Update survival GAS effects to match new need levels.
    updateSurvivalEffects(a.survival, a.system, a.vitals, a.tags, a.tuning);

    // Pre-scan: register decay entries for game-time resonance effects about to expire
    // (drugs with expiryMode="decay"). Must happen BEFORE tickGameTimeEffects removes them.
    {
        const u32 kOnyx = attr("onyx"), kAmber = attr("amber"), kGarnet = attr("garnet");
        for (const ActiveEffect& active : a.system.activeEffects) {
            if (!active.gameTime || active.infinite || !active.decayOnExpiry) continue;
            if (active.remaining <= gdt) {
                const f32 decayInit = (active.expiryMagnitude != 0.0f)
                    ? active.expiryMagnitude : active.magnitude;
                if (active.attribute == kOnyx || active.attribute == kAmber ||
                    active.attribute == kGarnet) {
                    a.resoDecays.list.push_back({
                        active.attribute, decayInit, active.decayPerHour, decayInit
                    });
                }
            }
        }
    }

    // Tick game-time GAS effects (drug duration, afflictions, injuries with durationHours).
    tickGameTimeEffects(a.vitals, a.system, gameDt, a.tags);

    // Resonance decay (game-time, in hours).
    tickResonanceDecays(a.resoDecays, static_cast<f32>(gameDt / 3600.0));

    // Injury recovery (game-time, in hours); re-sync GAS effects.
    const f32 restHours = static_cast<f32>(gameDt / 3600.0);
    recoverInjuries(a.injuries, restHours);
    syncInjuryEffects(a.injuries, a.system, a.vitals, a.tags);

    // Accumulate rest time.
    accrueRest(a.combat, gameDt);

    // Re-sync derived stats (maxHealth, maxEnergy, etc.) after all game-time
    // effects have ticked/expired. tickGameTimeEffects and syncInjuryEffects
    // call the 2-arg recomputeCurrent which lacks CoreAttributes and thus
    // overwrites derived targets with the raw AttributeSet field defaults (100).
    recomputeStats(a.core, a.vitals, a.resonance, a.system, a.derived, &mods);
}

GameTimeResult advanceGameTime(GameTimeTickArgs& a, f64 gameDt, f32 timescale,
                               const StatModifiers& equipmentMods) {
    constexpr f64 kChunkReal = 10.0;
    const f64 realTotal  = (timescale > 0.0f) ? gameDt / static_cast<f64>(timescale) : gameDt;
    f64 remaining = realTotal;

    while (remaining > 0.0) {
        const f64 thisReal = std::min(remaining, kChunkReal);
        const f64 thisGame = thisReal * static_cast<f64>(timescale);

        // 3-phase recompute.
        recomputeStats(a.core, a.vitals, a.resonance, a.system, a.derived, nullptr);
        const StatModifiers mods = buildCharacterMods(a, equipmentMods);
        recomputeStats(a.core, a.vitals, a.resonance, a.system, a.derived, &mods);

        const BuildupTickResult br =
            tickBuildup(a.buildup, a.system, static_cast<f32>(thisReal), a.tags, a.tuning);

        if (applyBuildupResult(a, br, a.derived, mods)) {
            return { true };
        }

        tickGameTime(a, thisGame, mods);
        remaining -= thisReal;
    }

    return { false };
}

} // namespace gameplay
