#include "gameplay/actors/CharacterTick.hpp"

#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/Attributes.hpp"
#include "gameplay/combat/Combat.hpp"
#include "gameplay/stats/Afflictions.hpp"
#include "gameplay/stats/CoreAttributes.hpp"
#include "gameplay/stats/Damage.hpp"
#include "gameplay/stats/Drugs.hpp"
#include "gameplay/stats/GameTime.hpp"
#include "gameplay/stats/Injuries.hpp"
#include "gameplay/stats/Resonance.hpp"
#include "gameplay/stats/StatusBuildup.hpp"
#include "gameplay/stats/Survival.hpp"

namespace gameplay {

void tickCharacter(flecs::entity entity, f32 dt, f64 gameDt,
                   const CharacterTickContext& ctx,
                   const StatModifiers& equipmentMods) {
    auto& core        = entity.get_mut<CoreAttributes>();
    auto& vitals      = entity.get_mut<AttributeSet>();
    auto& system      = entity.get_mut<AbilitySystem>();
    auto& combat      = entity.get_mut<CombatState>();
    auto& buildup     = entity.get_mut<StatusBuildup>();
    auto& resonance   = entity.get_mut<Resonance>();
    auto& survival    = entity.get_mut<Survival>();
    auto& activeDrugs = entity.get_mut<ActiveDrugs>();
    auto& injuries    = entity.get_mut<Injuries>();
    auto& afflictions = entity.get_mut<Afflictions>();

    // Stagger / paralysis timers decay in real time.
    updateStagger(combat, system, dt, ctx.tags);
    updateParalysis(combat, system, dt, ctx.tags);

    // Assemble full character modifiers and bake derived stats.
    GameTimeTickArgs args { core, vitals, system, combat, buildup, survival,
                            activeDrugs, injuries, afflictions, resonance,
                            ctx.afflictionDb, ctx.derived, ctx.tags, ctx.tuning };
    const StatModifiers mods = buildCharacterMods(args, equipmentMods);
    recomputeStats(core, vitals, system, ctx.derived, &mods);

    // Real-time status buildup: DoT, decay, status triggers.
    const BuildupTickResult br = tickBuildup(buildup, system, dt, ctx.tags, ctx.tuning);

    if (br.poisonHealthDamage > 0.0f)
        vitals.health = std::max(0.0f, vitals.health - br.poisonHealthDamage);
    if (br.ignitionHealthDamage > 0.0f)
        vitals.health = std::max(0.0f, vitals.health - br.ignitionHealthDamage);
    if (br.electrocutionEssenceDamage > 0.0f)
        vitals.essence = std::max(0.0f, vitals.essence - br.electrocutionEssenceDamage);
    if (br.bleedBurst) {
        StatBlock block { core, vitals, system, combat };
        applyDamage(block,
            DamageEvent { { { DamageType::Slash, ctx.tuning.bleedBurstDamage } }, 0.0f },
            ctx.tags, ctx.derived, &mods, ctx.tuning);
    }
    if (br.glaciationTriggered) {
        combat.paralysisSeconds = std::max(combat.paralysisSeconds,
                                           ctx.tuning.glaciationParalysisDuration);
        if (const auto paralyzed = ctx.tags.find("State.Paralyzed"))
            system.tags.add(*paralyzed, ctx.tags);
    }
    if (br.electrocutionTriggered) {
        const f32 maxP = currentValueOf(system, attr("maxPosture"));
        combat.posture = std::max(0.0f,
            combat.posture - maxP * ctx.tuning.electrocutionPostureDrainPercent);
        combat.staggerSeconds = std::max(combat.staggerSeconds, ctx.tuning.staggerSeconds);
        if (const auto staggered = ctx.tags.find("State.Staggered"))
            system.tags.add(*staggered, ctx.tags);
    }
    if (br.deathTriggered) {
        if (const auto dead = ctx.tags.find("State.Dead"))
            system.tags.add(*dead, ctx.tags);
    }

    // Real-time regen (posture while not staggered; energy always).
    // Regen-rate multipliers from active statuses are already in mods → currentValueOf.
    if (combat.staggerSeconds <= 0.0f) {
        const f32 maxP = currentValueOf(system, attr("maxPosture"));
        const f32 regen = currentValueOf(system, attr("postureRegen"));
        combat.posture = std::min(maxP, combat.posture + regen * dt);
    }
    {
        const f32 maxE = currentValueOf(system, attr("maxEnergy"));
        const f32 regen = currentValueOf(system, attr("energyRegen"));
        vitals.energy = std::min(maxE, vitals.energy + regen * dt);
    }

    // Game-time: health/essence regen, survival decay, drug expiry, injury recovery.
    tickGameTime(args, gameDt, mods);
}

void initializeActorStats(flecs::entity entity,
                          const CharacterTickContext& ctx,
                          const StatModifiers& equipmentMods) {
    auto& core        = entity.get_mut<CoreAttributes>();
    auto& vitals      = entity.get_mut<AttributeSet>();
    auto& system      = entity.get_mut<AbilitySystem>();
    auto& combat      = entity.get_mut<CombatState>();
    auto& buildup     = entity.get_mut<StatusBuildup>();
    auto& survival    = entity.get_mut<Survival>();
    auto& activeDrugs = entity.get_mut<ActiveDrugs>();
    auto& injuries    = entity.get_mut<Injuries>();
    auto& afflictions = entity.get_mut<Afflictions>();
    auto& resonance   = entity.get_mut<Resonance>();

    GameTimeTickArgs args { core, vitals, system, combat, buildup, survival,
                            activeDrugs, injuries, afflictions, resonance,
                            ctx.afflictionDb, ctx.derived, ctx.tags, ctx.tuning };
    const StatModifiers mods = buildCharacterMods(args, equipmentMods);
    recomputeStats(core, vitals, system, ctx.derived, &mods);
    vitals.health  = currentValueOf(system, attr("maxHealth"));
    vitals.energy  = currentValueOf(system, attr("maxEnergy"));
    vitals.essence = currentValueOf(system, attr("maxEssence"));
    recomputeStats(core, vitals, system, ctx.derived, &mods);
    combat.posture = currentValueOf(system, attr("maxPosture"));
}

} // namespace gameplay
