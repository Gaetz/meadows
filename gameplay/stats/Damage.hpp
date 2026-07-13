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

// The type's name, matching the enum spelling — tag composition
// ("Cue.Hit." + name, the C2 cue emission points) and logs.
const char* damageTypeName(DamageType type);

class DerivedStatRegistry;
struct StatBlock;
struct StatModifiers;
struct StatsTuningForm;
class GameplayTagRegistry;

// P0 D2a — an outright kill through the NORMAL pipeline (kill-z, future
// scripted executions): a damage event no mitigation survives, so death
// flows exactly like any other (life state, OnDeath, persistence).
void killOutright(StatBlock& target, const GameplayTagRegistry& tags,
                  const DerivedStatRegistry& derived,
                  const StatsTuningForm& tuning);

// Fall damage (D-catalogue leftover, 2026-07-13): blunt damage for a
// landing after `fallHeight` meters. 0 below tuning.fallMinHeight, then
// linear per meter. At/past tuning.fallLethalHeight the caller uses
// killOutright instead — no curve survives that. Pure and headless.
f32 fallDamage(f32 fallHeight, const StatsTuningForm& tuning);

struct DamageChannel {
    DamageType type { DamageType::Slash };
    f32 amount { 0.0f };
};

struct DamageEvent {
    vector<DamageChannel> channels;
    f32 postureAmount { 0.0f };
    // Chantier 6 C1: attacker-side offensive stats, carried on the event
    // (plain runtime struct — no ordinal concern). Pens subtract from the
    // target's POSITIVE mitigation only (they reduce protection, they
    // never amplify a vulnerability); `critical` triggers the critical
    // execution (criticalSensitivity% of the TARGET's maxHealth ×
    // `criticalMultiplier`, bypassing armor — docs/STATS.md §4).
    f32 armorPenetration { 0.0f };
    f32 resistPenetration { 0.0f };
    bool critical { false };
    f32 criticalMultiplier { 1.5f };
};

struct DamageResult {
    f32 healthDamage { 0.0f };  // mitigated total dealt to health
    f32 postureDamage { 0.0f }; // dealt to posture
    bool staggered { false };   // posture hit 0 this hit
};

// Combat resource state. `posture` is the poise resource (seeded to
// maxPosture); `staggerSeconds` counts down a stagger; `restSeconds` is the
// in-game time since the last hit ("Rest", §5 — the precondition for
// injury/resonance recovery; a hit resets it). Reflected since chantier 5:
// the save layer captures it by field name like the other stat components —
// chantier 6 fields are APPENDED (binary ordinals stable).
struct CombatState {
    f32 posture { 0.0f };
    f32 staggerSeconds { 0.0f };   // posture break: brief loss of control
    f32 paralysisSeconds { 0.0f }; // glaciation: frozen in place (distinct from stagger)
    f32 restSeconds { 0.0f };
    // Chantier 6 C2 (docs/STATS.md §4): the critical window opened by a
    // posture break — posture SITS at 0 until it elapses, then refills;
    // `shakenSeconds` is the short accuracy debuff from a heavy posture hit.
    f32 critWindowSeconds { 0.0f };
    f32 shakenSeconds { 0.0f };
    // FOLLOWERS É3 (APPENDED — ordinals stable): the bleedout window while
    // State.Downed (an active follower at 0 HP goes DOWN, not dead — the
    // updateLifeState routing). Counts down in updateDowned; the caller
    // resolves the timeout (recover with an injury / real death roll).
    f32 downedSeconds { 0.0f };

    REFLECT_BEGIN(CombatState, void)
        REFLECT_FIELD(posture)
        REFLECT_FIELD(staggerSeconds)
        REFLECT_FIELD(paralysisSeconds)
        REFLECT_FIELD(restSeconds)
        REFLECT_FIELD(critWindowSeconds)
        REFLECT_FIELD(shakenSeconds)
        REFLECT_FIELD(downedSeconds)
    REFLECT_END()
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

// Counts down the critical window; on expiry drops State.CriticalWeakness
// and refills posture to maxPosture (it sat at 0 for the whole window).
void updateCritWindow(CombatState& combat, AbilitySystem& system, f32 dt,
                      const GameplayTagRegistry& tags);

// Counts down the shaken debuff; drops State.Shaken when it elapses.
void updateShaken(CombatState& combat, AbilitySystem& system, f32 dt,
                  const GameplayTagRegistry& tags);

// FOLLOWERS É3 — counts down the bleedout window while State.Downed is
// held (the updateStagger timer pattern). Unlike the other timers it does
// NOT drop the tag itself: it returns true ONCE when the window elapses
// and the CALLER resolves the outcome (gameplay::resolveBleedout — a
// recovery revives above 0 HP and updateLifeState drops the tag; a real
// death lifts the protection and the same single write point kills).
bool updateDowned(CombatState& combat, AbilitySystem& system, f32 dt,
                  const GameplayTagRegistry& tags);

} // namespace gameplay
