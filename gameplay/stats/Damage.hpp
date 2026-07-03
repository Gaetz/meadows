#pragma once

#include "engine/core/Defines.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/DerivedStats.hpp"
#include "gameplay/ability/GameplayTags.hpp"
#include "gameplay/stats/CoreAttributes.hpp"
#include "gameplay/stats/StatsTuning.hpp"

// Typed damage + posture/stagger (docs/STATS.md §3-§4). The mitigation pipeline is
// the "custom execution calculation" the GAS deferred (§6): per channel, a flat
// reduction (defense / will, capped) then a percentage reduction (armor /
// resistance), summed onto health. Posture is a combat resource (not a GAS
// attribute); depleting it staggers the actor.

namespace gameplay {

class DerivedStatRegistry;

// Physical (slash/pierce/blunt) + elemental. The nine elements map to the three
// essence attributes (docs/STATS.md §3): charisma → fire/sonic/holy,
// ego → cold/chemical/dark, insight → lightning/psychic/ether.
enum class DamageType {
    Slash, Pierce, Blunt,
    Fire, Cold, Lightning,
    Sonic, Chemical, Psychic, Holy, Dark, Ether,
};

struct DamageChannel {
    DamageType type { DamageType::Slash };
    f32 amount { 0.0f };
};

struct DamageEvent {
    vector<DamageChannel> channels;
    f32 postureAmount { 0.0f };
};

struct DamageResult {
    f32 healthDamage { 0.0f };  // mitigated total dealt to health
    f32 postureDamage { 0.0f }; // dealt to posture
    bool staggered { false };   // posture hit 0 this hit
};

// Combat resource state (runtime, not reflected; like AbilitySystem). `posture`
// is the poise resource (seeded to maxPosture); `staggerSeconds` counts down a
// stagger; `restSeconds` is the in-game time since the last hit ("Rest", §5 — the
// precondition for injury/resonance recovery; a hit resets it). Critical-weakness
// / shaken live here too in Phase 7.
struct CombatState {
    f32 posture { 0.0f };
    f32 staggerSeconds { 0.0f };   // posture break: opens criticals
    f32 paralysisSeconds { 0.0f }; // glaciation: frozen in place (distinct from stagger)
    f32 restSeconds { 0.0f };
};

// The bundle of stat components an actor carries — passed to the execution
// functions. In the ECS these come from separate component storages (the scene
// builds a block per actor).
struct StatBlock {
    CoreAttributes& core;
    AttributeSet& vitals;
    AbilitySystem& system;
    CombatState& combat;
};

// Applies a typed damage event to the target: mitigates each channel, deals the
// total to health, deals posture damage, recomputes, and staggers on a posture
// break (granting State.Staggered). Reads the target's derived defenses from the
// overlay, so the stats must have been recomputed first. Deterministic (no rolls).
DamageResult applyDamage(StatBlock& target, const DamageEvent& event,
                         const GameplayTagRegistry& tags,
                         const DerivedStatRegistry& derived,
                         const StatModifiers* extra = nullptr,
                         const StatsTuningForm& tuning = {});

// Counts down an active stagger; drops State.Staggered when it elapses.
void updateStagger(CombatState& combat, AbilitySystem& system, f32 dt,
                   const GameplayTagRegistry& tags);

// Counts down glaciation paralysis; drops State.Paralyzed when it elapses.
void updateParalysis(CombatState& combat, AbilitySystem& system, f32 dt,
                     const GameplayTagRegistry& tags);

} // namespace gameplay
