#include "gameplay/actors/CharacterTick.hpp"

#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/Attributes.hpp"
#include "gameplay/combat/Combat.hpp"
#include "gameplay/stats/Afflictions.hpp"
#include "gameplay/stats/CoreAttributes.hpp"
#include "gameplay/stats/Damage.hpp"
#include "gameplay/stats/GameTime.hpp"
#include "gameplay/stats/Injuries.hpp"
#include "gameplay/stats/Resonance.hpp"
#include "gameplay/stats/ResonanceDecays.hpp"
#include "gameplay/stats/StatusBuildup.hpp"
#include "gameplay/stats/Survival.hpp"

namespace gameplay {

namespace {

// Publish the Phase-A primary maxima — the derived
// formulas over BASE attributes plus plain GAS modifiers, BEFORE the
// resonance/cascade mods scale them — under synthetic overlay ids (the
// `damage` meta-attribute precedent: an entry without a reflected
// field). The HUD sizes its bars from these instead of re-deriving the
// theoretical max as effectiveMax / (1 + r/100) — the sim's rule stays
// in ONE place (buildResonanceModifiers).
void publishTheoreticalMaxima(AbilitySystem& system) {
    system.current[attr("theoreticalMaxHealth")] =
        currentValueOf(system, attr("maxHealth"));
    system.current[attr("theoreticalMaxEnergy")] =
        currentValueOf(system, attr("maxEnergy"));
    system.current[attr("theoreticalMaxEssence")] =
        currentValueOf(system, attr("maxEssence"));
}

} // namespace

void tickCharacter(ecs::Entity entity, f32 dt, f64 gameDt,
                   const CharacterTickContext& ctx,
                   const StatModifiers& equipmentMods) {
    auto& core        = entity.get_mut<CoreAttributes>();
    auto& vitals      = entity.get_mut<AttributeSet>();
    auto& system      = entity.get_mut<AbilitySystem>();
    auto& combat      = entity.get_mut<CombatState>();
    auto& buildup     = entity.get_mut<StatusBuildup>();
    auto& resonance   = entity.get_mut<Resonance>();
    auto& survival    = entity.get_mut<Survival>();
    auto& injuries    = entity.get_mut<Injuries>();
    auto& resoDecays  = entity.get_mut<ResonanceDecays>();

    // Stagger / paralysis / crit-window / shaken timers decay in real time.
    updateStagger(combat, system, dt, ctx.tags);
    updateParalysis(combat, system, dt, ctx.tags);
    updateCritWindow(combat, system, dt, ctx.tags);
    updateShaken(combat, system, dt, ctx.tags);

    // Pre-register decay entries for real-time resonance effects about to expire.
    {
        const u32 kOnyx = attr("onyx"), kAmber = attr("amber"), kGarnet = attr("garnet");
        for (const ActiveEffect& active : system.activeEffects) {
            if (active.gameTime || active.infinite || !active.decayOnExpiry) continue;
            if (active.remaining <= dt) {
                if (active.attribute == kOnyx || active.attribute == kAmber ||
                    active.attribute == kGarnet) {
                    const f32 decayInit = (active.expiryMagnitude != 0.0f)
                        ? active.expiryMagnitude : active.magnitude;
                    resoDecays.list.push_back({
                        active.attribute, decayInit, active.decayPerHour, decayInit
                    });
                }
            }
        }
    }

    // Tick real-time GAS effects (cooldowns, duration effects, tag expiry).
    tickEffects(vitals, system, dt, ctx.tags);

    // Phase A — resonance current values: Resonance BaseValues + GAS activeEffects.
    recomputeStats(core, vitals, resonance, system, ctx.derived, nullptr);
    publishTheoreticalMaxima(system); // pre-resonance maxima, for the HUD

    // Phase B — full character mods from GAS resonance + cascade + equipment.
    GameTimeTickArgs args { core, vitals, system, combat, buildup, survival,
                            injuries, resonance, resoDecays,
                            ctx.derived, ctx.tags, ctx.tuning };
    const StatModifiers mods = buildCharacterMods(args, equipmentMods);

    // Phase C — full recompute with cascade mods (maxHealth, attributes, derived).
    recomputeStats(core, vitals, resonance, system, ctx.derived, &mods);

    // Real-time status buildup: DoT, decay, status triggers.
    const BuildupTickResult br = tickBuildup(buildup, system, dt, ctx.tags, ctx.tuning);

    // Buildup consequences (DoT drains, bleed burst, status triggers, lethal
    // zeroing + life-state sync) — the ONE shared implementation with the
    // time-skip path. The return (died) is not consumed
    // here: the health regen it would guard is gated by the State.Dead tag
    // this call just synced (tickGameTime's isDead gate).
    applyBuildupResult(args, br, mods);

    // Real-time regen (posture while not staggered NOR in the critical
    // window — posture must sit at 0 for the whole window; energy always).
    if (combat.staggerSeconds <= 0.0f && combat.critWindowSeconds <= 0.0f) {
        const f32 maxP = currentValueOf(system, attr("maxPosture"));
        const f32 regen = currentValueOf(system, attr("postureRegen"));
        combat.posture = std::min(maxP, combat.posture + regen * dt);
    }
    {
        // §2.9 execution calc: rate-driven regen (dynamic captured magnitude +
        // gates) — no static EffectForm can express this; fills BaseValue.
        const f32 maxE = currentValueOf(system, attr("maxEnergy"));
        const f32 regen = currentValueOf(system, attr("energyRegen"));
        // Post-spend recharge delay: spending energy pauses regen for a beat
        // (set in applyEffect). Count it down; only regen once it elapses.
        if (system.energyRegenDelay > 0.0f) {
            system.energyRegenDelay = std::max(0.0f, system.energyRegenDelay - dt);
        } else {
            // STATS.md §4: energy regen halves while the guard is up
            // (holding the parry is effortful, turtling starves you).
            f32 rate = regen;
            if (const auto blocking = ctx.tags.find("State.Blocking");
                blocking && system.tags.has(*blocking)) {
                rate *= ctx.tuning.blockEnergyRegenFactor;
            }
            vitals.energy = std::min(maxE, vitals.energy + rate * dt);
        }

        // Exhaustion gate: energy-costed abilities (dodge, attack, sprint) carry
        // blockedTag=State.Exhausted, so tryActivate rejects them while out of
        // energy. Shared here so enemies gate too (hysteresis lives in the
        // helper). Threshold is a constant for now (→ StatsTuningForm later).
        updateExhaustion(vitals, system, ctx.tags);
    }

    // Game-time: health/essence regen, survival decay, drug expiry, injury recovery.
    tickGameTime(args, gameDt, mods);
}

void initializeActorStats(ecs::Entity entity,
                          const CharacterTickContext& ctx,
                          const StatModifiers& equipmentMods) {
    auto& core        = entity.get_mut<CoreAttributes>();
    auto& vitals      = entity.get_mut<AttributeSet>();
    auto& system      = entity.get_mut<AbilitySystem>();
    auto& combat      = entity.get_mut<CombatState>();
    auto& buildup     = entity.get_mut<StatusBuildup>();
    auto& survival    = entity.get_mut<Survival>();
    auto& injuries    = entity.get_mut<Injuries>();
    auto& resonance   = entity.get_mut<Resonance>();
    auto& resoDecays  = entity.get_mut<ResonanceDecays>();

    // Sync initial injury GAS effects.
    syncInjuryEffects(injuries, system, vitals, ctx.tags);

    // Sync initial survival GAS effects.
    updateSurvivalEffects(survival, system, vitals, ctx.tags, ctx.tuning);

    // Phase A — resonance values from GAS (no cascade yet).
    recomputeStats(core, vitals, resonance, system, ctx.derived, nullptr);
    publishTheoreticalMaxima(system); // seeded before the first HUD read

    GameTimeTickArgs args { core, vitals, system, combat, buildup, survival,
                            injuries, resonance, resoDecays,
                            ctx.derived, ctx.tags, ctx.tuning };
    const StatModifiers mods = buildCharacterMods(args, equipmentMods);

    // Phase C — full recompute with cascade mods.
    recomputeStats(core, vitals, resonance, system, ctx.derived, &mods);

    // Restore vitals to full, then recompute once more so derived stats are
    // consistent with the new maxima.
    vitals.health  = currentValueOf(system, attr("maxHealth"));
    vitals.energy  = currentValueOf(system, attr("maxEnergy"));
    vitals.essence = currentValueOf(system, attr("maxEssence"));
    recomputeStats(core, vitals, resonance, system, ctx.derived, &mods);
    combat.posture = currentValueOf(system, attr("maxPosture"));

    // Clear State.Dead if health was just restored above 0.
    updateLifeState(system, ctx.tags);
}

} // namespace gameplay
