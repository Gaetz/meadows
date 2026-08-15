#include "gameplay/stats/GameTime.hpp"

#include <algorithm>

#include "gameplay/ability/GameplayEffects.hpp" // tickGameTimeEffects
#include "gameplay/combat/Combat.hpp"            // updateLifeState
#include "gameplay/stats/GameClock.hpp"
#include "gameplay/stats/Rest.hpp"               // accrueRest
#include "gameplay/stats/ResonanceDecays.hpp"    // tickResonanceDecays

namespace gameplay {

namespace {

bool isDead(const GameTimeTickArgs& a) {
    const auto dead = a.tags.find("State.Dead");
    return dead && a.system.tags.has(*dead);
}

// A DOWNED actor must not regenerate either — health
// creeping over 0 would silently stand him back up mid-bleedout (the
// revive/bleedout paths own the exit from Downed, exactly like the
// corpse-regen gate above owns Dead).
bool isDowned(const GameTimeTickArgs& a) {
    const auto downed = a.tags.find("State.Downed");
    return downed && a.system.tags.has(*downed);
}

} // namespace

bool applyBuildupResult(GameTimeTickArgs& a, const BuildupTickResult& br,
                        const StatModifiers& mods) {
    // §2.9 execution calc: buildup DoT / lethal zeroing drain BaseValues
    // directly (final per-tick amounts — resistance acts on buildup
    // accumulation, not the tick). ONE implementation for the real-time
    // (tickCharacter) and time-skip (advanceGameTime) paths.
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
        // Bleed = one critical-sensitivity chunk of max health, ignoring armor
        // (docs/STATS.md §4); the weapon hit already dealt its mitigated damage.
        DamageEvent bleed;
        bleed.critical = true;
        bleed.criticalMultiplier = 1.0f;
        applyDamage(block, bleed, a.tags, a.derived, &mods, a.tuning);
    }
    if (br.electrocutionTriggered) {
        const f32 maxP = currentValueOf(a.system, attr("maxPosture"));
        a.combat.posture = std::max(0.0f,
            a.combat.posture - maxP * a.tuning.electrocutionPostureDrainPercent);
        // The jolt also breaks the stance — on BOTH the real-time and
        // time-skip paths.
        a.combat.staggerSeconds =
            std::max(a.combat.staggerSeconds, a.tuning.staggerSeconds);
        if (const auto staggered = a.tags.find("State.Staggered")) {
            a.system.tags.add(*staggered, a.tags);
        }
    }
    if (br.glaciationTriggered) {
        a.combat.paralysisSeconds = std::max(
            a.combat.paralysisSeconds, a.tuning.glaciationParalysisDuration);
        if (const auto tag = a.tags.find("State.Paralyzed")) {
            a.system.tags.add(*tag, a.tags);
        }
    }
    if (br.deathTriggered) {
        // Lethal zeroing writes the BASE (§2.9): setting the State.Dead
        // tag alone would leave health > 0, so the next
        // life-state sync would resurrect the actor and it would reload
        // ALIVE across a save.
        a.vitals.health = 0.0f;
    }
    if (a.vitals.health <= 0.0f) {
        // The current overlay still holds the pre-drain health and
        // updateLifeState reads CURRENT — refresh it first, or the death
        // tag lags a full tick behind the kill.
        recomputeCurrent(a.vitals, a.system);
        updateLifeState(a.system, a.tags);
        return true;
    }
    return false;
}

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
    // Health and essence regen (game-time; very slow — docs/STATS.md §3). A dead
    // actor NEVER regenerates: health creeping back over 0 would silently revive
    // the corpse, and — worse — that regenerated BASE health persists, so a slain
    // NPC reloads ALIVE across a cell unload/reload (the save layer re-derives the
    // life state from health, §5/§6). This is the gate `isDead` was written for.
    // Downed gates it too — no silent self-revive mid-bleedout.
    if (!isDead(a) && !isDowned(a)) {
        a.vitals.health  = std::min(cur("maxHealth"),  a.vitals.health  + cur("healthRegen")  * gdt);
        a.vitals.essence = std::min(cur("maxEssence"), a.vitals.essence + cur("essenceRegen") * gdt);
    }

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

    const f32 gameHours = static_cast<f32>(gameDt / 3600.0);

    // Resonance decay (game-time, in hours).
    tickResonanceDecays(a.resoDecays, gameHours);

    // Injury recovery: only over REST — game time without taking a hit
    // (docs/STATS.md §5). A fresh hit zeroes restSeconds and pauses healing.
    recoverInjuries(a.injuries,
                    a.combat.restSeconds > 0.0f ? gameHours : 0.0f);
    syncInjuryEffects(a.injuries, a.system, a.vitals, a.tags);

    // Accumulate rest time.
    accrueRest(a.combat, gameDt);

    // Re-run the derived formulas after all game-time effects have
    // ticked/expired (the partial recomputes above PRESERVE the previous
    // derived currents — but only a full recompute refreshes
    // them against the new modifier set).
    recomputeStats(a.core, a.vitals, a.resonance, a.system, a.derived, &mods);
}

GameTimeResult waitGameTime(GameClock& clock, GameTimeTickArgs& a, f32 hours,
                            const StatModifiers& equipmentMods) {
    const f64 gameDt = static_cast<f64>(hours) * 3600.0;
    clock.gameSeconds += gameDt;
    // The window is rest by definition — nothing hits a waiting actor;
    // credited BEFORE the skip so injury recovery sees it.
    accrueRest(a.combat, gameDt);
    return advanceGameTime(a, gameDt, clock.timescale, equipmentMods);
}

GameTimeResult sleepGameTime(GameClock& clock, GameTimeTickArgs& a, f32 hours,
                             const StatModifiers& equipmentMods) {
    const f32 sleepBefore = a.survival.sleep;
    const GameTimeResult result =
        waitGameTime(clock, a, hours, equipmentMods);
    // Restores the need the skip's survival decay just drained. AFTER the
    // skip, or an 8h night would wake below full.
    a.survival.sleep =
        hours >= a.tuning.comfortableSleepHours
            ? 100.0f
            : std::min(100.0f,
                       sleepBefore + a.tuning.sleepPerHour * hours);
    return result;
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

        if (applyBuildupResult(a, br, mods)) {
            return { true };
        }

        tickGameTime(a, thisGame, mods);
        remaining -= thisReal;
    }

    return { false };
}

} // namespace gameplay
