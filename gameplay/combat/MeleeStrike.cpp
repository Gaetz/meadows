#include "gameplay/combat/MeleeStrike.hpp"

#include "gameplay/cue/GameplayCues.hpp"
#include "gameplay/event/EventBus.hpp"

namespace gameplay {

DamageResult resolveStrikeDamage(StatBlock& defender,
                                 ecs::Entity attackerEntity,
                                 ecs::Entity defenderEntity,
                                 const DamageEvent& event,
                                 const Vec3& impact,
                                 const StrikeContext& ctx, bool blocked) {
    const DamageResult result = applyDamage(defender, event, ctx.tags,
                                            ctx.derived, nullptr,
                                            ctx.tuning);
    // Combat lifecycle events (BOSS-SCRIPTING §1) — quests, cues and
    // brains listen on the bus.
    if (ctx.bus) {
        Event hit;
        hit.kind = eventKind("OnHitTaken");
        hit.source = attackerEntity;
        hit.target = defenderEntity;
        hit.value = result.healthDamage;
        ctx.bus->dispatch(hit);
        if (result.staggered) {
            ctx.bus->dispatch({ eventKind("OnStagger"), attackerEntity,
                                defenderEntity });
        }
    }
    // The LOOK of the exchange — one cue per outcome, resolved
    // through CueForms (data): a block beats a plain hit.
    if (ctx.cues) {
        if (blocked) {
            ctx.cues->emit({ "Cue.Block", impact, result.postureDamage });
        } else {
            const DamageType type = event.channels.empty()
                                        ? DamageType::Slash
                                        : event.channels[0].type;
            ctx.cues->emit({ str { "Cue.Hit." } + damageTypeName(type),
                             impact, result.healthDamage });
        }
    }
    return result;
}

StrikeOutcome resolveMeleeStrike(StatBlock& attacker, StatBlock& defender,
                                 ecs::Entity attackerEntity,
                                 ecs::Entity defenderEntity,
                                 DamageEvent event,
                                 const StrikeGeometry& geo,
                                 const StrikeContext& ctx) {
    StrikeOutcome out;
    // Sneak attack: a State.Sneaking
    // attacker striking an UNAWARE defender multiplies every channel and
    // the posture hit (sneakAttackMultiplier — §5 moddable). An unaware
    // defender has no guard up in practice, so the block stage below
    // stays untouched.
    if (geo.targetUnaware) {
        if (const auto sneaking = ctx.tags.find("State.Sneaking");
            sneaking && attacker.system.tags.has(*sneaking)) {
            out.sneakAttack = true;
            for (DamageChannel& channel : event.channels) {
                channel.amount *= ctx.tuning.sneakAttackMultiplier;
            }
            event.postureAmount *= ctx.tuning.sneakAttackMultiplier;
        }
    }
    // A defender in its critical window eats the critical execution
    // (both camps go through this one path).
    if (const auto weakness = ctx.tags.find("State.CriticalWeakness")) {
        event.critical = defender.system.tags.has(*weakness);
        out.critical = event.critical;
    }
    // A raised guard catches front-cone hits — damage shrinks, the
    // blocked amount runs the guard's POSTURE down instead. A guard
    // raised inside the perfect window parries CLEAN and the ATTACKER's
    // poise pays for the read attack.
    if (const auto blockTag = ctx.tags.find("State.Blocking");
        blockTag && defender.system.tags.has(*blockTag)) {
        out.guard = applyBlock(
            event, geo.defenderFacing, geo.defenderPos, geo.attackerPos,
            ctx.tuning.blockAngleDegrees, ctx.tuning.blockFactor,
            ctx.tuning.blockPostureFactor, geo.defenderGuardSeconds,
            ctx.tuning.perfectParryWindow,
            currentValueOf(defender.system, attr("energy")),
            // STATS.md §4: the empty-guard punish.
            currentValueOf(defender.system, attr("maxPosture")) *
                currentValueOf(defender.system,
                               attr("criticalSensitivity")) /
                100.0f);
    }
    if (out.guard.perfect) {
        DamageEvent parry;
        parry.postureAmount = ctx.tuning.perfectParryPosture;
        out.riposte = applyDamage(attacker, parry, ctx.tags, ctx.derived,
                                  nullptr, ctx.tuning);
        if (ctx.bus) {
            // source = the parrier, target = the parried.
            ctx.bus->dispatch({ eventKind("OnParried"), defenderEntity,
                                attackerEntity });
            if (out.riposte.staggered) {
                ctx.bus->dispatch({ eventKind("OnStagger"),
                                    defenderEntity, attackerEntity });
            }
        }
        if (ctx.cues) {
            ctx.cues->emit({ "Cue.Parry", geo.impact,
                             ctx.tuning.perfectParryPosture });
        }
        return out; // nothing lands on the defender — clean catch
    }
    out.damage = resolveStrikeDamage(defender, attackerEntity,
                                     defenderEntity, event, geo.impact,
                                     ctx, out.guard.caught);
    return out;
}

} // namespace gameplay
