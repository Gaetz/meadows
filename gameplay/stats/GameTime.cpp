#include "gameplay/stats/GameTime.hpp"

#include <algorithm>

#include "gameplay/combat/Combat.hpp"     // updateLifeState
#include "gameplay/stats/Rest.hpp"        // accrueRest

namespace gameplay {

namespace {

// Builds the full StatModifiers for a character from all resonance-driven sources
// (resonance spheres, survival, injuries, afflictions, drugs) merged with equipmentMods.
// Does NOT include equipment (pass those via equipmentMods).
StatModifiers buildCharacterMods(GameTimeTickArgs& a, const StatModifiers& equipmentMods) {
    Resonance eff = effectiveResonance(a.resonance, a.survival, a.tuning);
    eff.onyx  += injuryResonance(a.injuries);
    const Resonance ar = afflictionResonance(a.afflictions, a.afflictionDb);
    eff.amber  += ar.amber;
    eff.garnet += ar.garnet;
    const Resonance dr = drugResonance(a.activeDrugs);
    eff.onyx   += dr.onyx;
    eff.amber  += dr.amber;
    eff.garnet += dr.garnet;
    const Resonance da = drugAftereffectResonance(a.activeDrugs);
    eff.onyx   += da.onyx;
    eff.amber  += da.amber;
    eff.garnet += da.garnet;

    StatModifiers mods;
    buildResonanceModifiers(eff, mods, harmonyBroken(a.system, a.tags));
    injuryStatModifiers(a.injuries, mods);
    afflictionStatModifiers(a.afflictions, a.afflictionDb, mods);

    // Merge equipment / fixed mods.
    for (const auto& [k, v] : equipmentMods.add) { mods.add[k] += v; }
    for (const auto& [k, v] : equipmentMods.mul) {
        auto [it, inserted] = mods.mul.try_emplace(k, 1.0f);
        it->second *= v;
    }

    buildupStatusModifiers(a.system, a.tags, a.tuning, mods);
    return mods;
}

bool isDead(const GameTimeTickArgs& a) {
    const auto dead = a.tags.find("State.Dead");
    return dead && a.system.tags.has(*dead);
}

// Applies the per-chunk buildup result (damages + triggers) inline.
// Returns true if the actor died as a result.
bool applyBuildupResult(GameTimeTickArgs& a, const BuildupTickResult& br,
                        const DerivedStatRegistry& derived, const StatModifiers& mods) {
    // Ongoing DoT.
    if (br.poisonHealthDamage > 0.0f || br.ignitionHealthDamage > 0.0f) {
        a.vitals.health = std::max(0.0f,
            a.vitals.health - br.poisonHealthDamage - br.ignitionHealthDamage);
    }
    if (br.electrocutionEssenceDamage > 0.0f) {
        a.vitals.essence = std::max(0.0f,
            a.vitals.essence - br.electrocutionEssenceDamage);
    }

    // Bleed burst: apply slash damage via the full pipeline.
    if (br.bleedBurst) {
        StatBlock block { a.core, a.vitals, a.system, a.combat };
        applyDamage(block,
            DamageEvent { { { DamageType::Slash, a.tuning.bleedBurstDamage } }, 0.0f },
            a.tags, derived, &mods, a.tuning);
    }

    // Electrocution: posture drain.
    if (br.electrocutionTriggered) {
        const f32 maxP = currentValueOf(a.system, attr("maxPosture"));
        a.combat.posture = std::max(0.0f,
            a.combat.posture - maxP * a.tuning.electrocutionPostureDrainPercent);
    }

    // Glaciation: paralysis timer (real-time, expires naturally in update()).
    if (br.glaciationTriggered) {
        a.combat.paralysisSeconds = std::max(
            a.combat.paralysisSeconds, a.tuning.glaciationParalysisDuration);
        if (const auto tag = a.tags.find("State.Paralyzed")) {
            a.system.tags.add(*tag, a.tags);
        }
    }

    // Instant death trigger from the death buildup.
    if (br.deathTriggered) {
        a.vitals.health = 0.0f;
    }

    // Death check (health 0 → State.Dead).
    if (a.vitals.health <= 0.0f) {
        updateLifeState(a.system, a.tags);
        return true;
    }
    return false;
}

} // namespace

void tickGameTime(GameTimeTickArgs& a, f64 gameDt, const StatModifiers& mods) {
    const auto cur = [&](const char* n) { return currentValueOf(a.system, attr(n)); };
    const f32 gdt = static_cast<f32>(gameDt);

    // Health and essence regen (game-time; very slow — docs/STATS.md §3).
    // Regen-rate modifiers (electrocution suppresses essence, glaciation reduces energy, etc.)
    // are already baked into currentValueOf via buildupStatusModifiers → recomputeStats.
    a.vitals.health  = std::min(cur("maxHealth"),  a.vitals.health  + cur("healthRegen")  * gdt);
    a.vitals.essence = std::min(cur("maxEssence"), a.vitals.essence + cur("essenceRegen") * gdt);

    // Survival needs decay (game-time).
    tickSurvival(a.survival, gameDt, a.tuning);

    // Drug expiry and progressive aftereffect (game-time).
    tickDrugs(a.activeDrugs, a.system, gameDt, a.tags);

    // Injury and affliction recovery (game-time, in hours).
    recoverInjuries(a.injuries, static_cast<f32>(gameDt / 3600.0));
    recoverAfflictions(a.afflictions, static_cast<f32>(gameDt / 3600.0));

    // Accumulate rest time.
    accrueRest(a.combat, gameDt);
}

GameTimeResult advanceGameTime(GameTimeTickArgs& a, f64 gameDt, f32 timescale,
                               const StatModifiers& equipmentMods) {
    constexpr f64 kChunkReal = 10.0; // 10 real seconds per chunk
    const f64 realTotal  = (timescale > 0.0f) ? gameDt / static_cast<f64>(timescale) : gameDt;
    f64 remaining = realTotal;

    while (remaining > 0.0) {
        const f64 thisReal = std::min(remaining, kChunkReal);
        const f64 thisGame = thisReal * static_cast<f64>(timescale);

        // Recompute mods and overlay from current state.
        const StatModifiers mods = buildCharacterMods(a, equipmentMods);
        recomputeStats(a.core, a.vitals, a.system, a.derived, &mods);

        // Tick status buildup in real time (DoT + decay + tag expiry).
        const BuildupTickResult br =
            tickBuildup(a.buildup, a.system, static_cast<f32>(thisReal), a.tags, a.tuning);

        // Apply damages and triggers; stop if the character died.
        if (applyBuildupResult(a, br, a.derived, mods)) {
            return { true };
        }

        // Apply game-time effects for this chunk.
        tickGameTime(a, thisGame, mods);

        remaining -= thisReal;
    }

    return { false };
}

} // namespace gameplay
