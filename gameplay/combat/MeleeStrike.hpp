#pragma once

#include "engine/core/Defines.hpp"
#include "engine/ecs/World.hpp"
#include "gameplay/combat/MeleeSwing.hpp" // BlockResult
#include "gameplay/stats/Damage.hpp"      // StatBlock, DamageEvent/Result

// ONE strike resolution for every camp. This module is the single owner
// of the block/parry/damage exchange rules — both controllers (player and
// NPC, plus the arrow path) delegate here, so the two camps cannot drift.
//
// Sim-pure: EventBus and CueRegistry are gameplay-layer, entities are only
// carried as event source/target (pass empty handles in headless tests).

namespace core {
class Rng;
}

namespace gameplay {

class EventBus;
class CueRegistry;

// Everything the rules read that is NOT per-strike: tag vocabulary,
// derived-stat overlay, tuning, and the optional feedback sinks.
struct StrikeContext {
    const GameplayTagRegistry& tags;
    const DerivedStatRegistry& derived;
    const StatsTuningForm& tuning;
    EventBus* bus { nullptr };
    CueRegistry* cues { nullptr };
    // Injury rolls on landed physical hits (docs/STATS.md §5), through
    // the seeded engine RNG (§8). Null = no rolls (headless tests keep
    // their deterministic damage numbers, benches opt out).
    core::Rng* rng { nullptr };
};

// The per-strike geometry the guard cone and the cues need.
struct StrikeGeometry {
    Vec3 attackerPos {};
    Vec3 defenderPos {};
    Vec3 defenderFacing {}; // defender rotation * +Z
    // The defender's MeleeSwing.guardSeconds (perfect-parry window);
    // < 0 = guard down.
    f32 defenderGuardSeconds { -1.0f };
    Vec3 impact {}; // cue anchor (the defender's chest)
    // Sneak attack: the defender never noticed
    // the attacker (world-layer Perception AwareState::Calm — the caller
    // passes the flat bool, the rules never see world types). Combined
    // with a State.Sneaking attacker it multiplies the strike.
    bool targetUnaware { false };
};

struct StrikeOutcome {
    BlockResult guard {};
    DamageResult damage {};  // landed on the DEFENDER (empty on a parry)
    DamageResult riposte {}; // perfect parry: the ATTACKER's poise hit
    bool critical { false }; // the crit execution fired (critical window)
    bool sneakAttack { false }; // the sneak multiplier applied
};

// The full melee exchange, both camps (docs/STATS.md §4):
//   1. a defender in its critical window eats the critical execution;
//   2. a raised guard (State.Blocking) catches front-cone hits via
//      applyBlock — including the perfect parry and the empty-guard
//      punish;
//   3. a PERFECT parry lands `perfectParryPosture` on the attacker's
//      poise instead (OnParried + OnStagger-if-broken + Cue.Parry) and
//      nothing on the defender;
//   4. otherwise the (possibly blocked) event runs resolveStrikeDamage.
// `event` is taken by value: applyBlock rewrites its channels.
StrikeOutcome resolveMeleeStrike(StatBlock& attacker, StatBlock& defender,
                                 ecs::Entity attackerEntity,
                                 ecs::Entity defenderEntity,
                                 DamageEvent event,
                                 const StrikeGeometry& geo,
                                 const StrikeContext& ctx);

// The tail every strike shares (melee after the guard stage, arrows
// directly): applyDamage -> OnHitTaken (+ OnStagger on a posture break)
// -> Cue.Block / Cue.Hit.<type>. `blocked` picks the block cue.
DamageResult resolveStrikeDamage(StatBlock& defender,
                                 ecs::Entity attackerEntity,
                                 ecs::Entity defenderEntity,
                                 const DamageEvent& event,
                                 const Vec3& impact,
                                 const StrikeContext& ctx,
                                 bool blocked = false);

} // namespace gameplay
