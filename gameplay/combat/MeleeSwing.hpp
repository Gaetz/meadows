#pragma once

#include <string_view>

#include "engine/core/Defines.hpp"

// Chantier P0 A3/A4 — the melee swing (docs/CHANTIER-P0.md, dev design
// 2026-07-11): the attack is an arc the VISIBLE blade travels, and damage
// lands only when that blade touches the target. One state machine and one
// hit test shared by the player (camera-driven socket) and the NPCs
// (Sword_Attack clip drives the hand; the sword rides the hand joint).
//
// Activation gates (energy cost, cooldown) are NOT here — they are the
// attack AbilityForm's job (tryActivate), per §6. This file owns what
// happens AFTER the ability fires.

namespace gameplay {

struct DamageEvent; // stats/Damage.hpp — mutated by applyBlock (A5)

// Idle -> Windup -> Active (the damaging sweep) -> Recovery -> Idle.
enum class SwingPhase : u8 { Idle, Windup, Active, Recovery };

// Per-phase durations, straight from WeaponForm (§5 moddable:
// swingWindup / swingActive / swingRecovery).
struct SwingTiming {
    f32 windup { 0.25f };
    f32 active { 0.20f };
    f32 recovery { 0.35f };
};

// Runtime-only component (registered in registerGameplayComponents, like
// AbilitySystem): transient per-swing state — a save mid-swing just drops
// the swing, nothing to persist.
struct MeleeSwing {
    SwingPhase phase { SwingPhase::Idle };
    f32 t { 0.0f };     // seconds into the current phase
    vector<u64> struck; // entities already hit THIS swing (once per swing)
};

// THE phase transition (dev rule: enum + flat switches, every change goes
// through one function). Resets the phase clock; entering Windup clears
// the struck set.
void setSwingPhase(MeleeSwing& swing, SwingPhase phase);

// Starts a swing if Idle (call after the ability activated). False = a
// swing is already in flight.
bool startSwing(MeleeSwing& swing);

// Data-timed phase advance. The anim-event override below can shortcut
// the windows when a clip carries authored events.
void updateSwing(MeleeSwing& swing, f32 dt, const SwingTiming& timing);

// AnimEvents authored on an attack clip drive the damage window instead
// of the data timings: "HitOpen" opens Active (from Windup), "HitClose"
// closes it (from Active). Anything else is ignored.
void onSwingAnimEvent(MeleeSwing& swing, std::string_view name);

// 0 -> 1 across the Active window: the sweep parameter.
f32 swingSweepT(const MeleeSwing& swing, const SwingTiming& timing);

// True exactly once per target per swing: records the strike.
bool registerStrike(MeleeSwing& swing, u64 targetId);

// The SIMULATED weapon socket (no attack clip — the player's first-person
// viewmodel): hand transform in actor-local space (X right, Y up,
// -Z forward, origin at the eye/chest anchor). The blade is the socket's
// +Y — the sword mesh grows along +Y from its grip (WeaponMeshes). Idle =
// guard pose bottom-right; windup arms to the right; active sweeps
// right -> left; recovery eases back to guard. Callers place it with
// `actorBasis * swingSocketLocal(...)`. Pose constants are [cpp-tuning:
// dev visual pass].
Mat4 swingSocketLocal(const MeleeSwing& swing, const SwingTiming& timing);

// Analytic blade-vs-actor test. CharacterVirtual bodies live OUTSIDE the
// Jolt broadphase — physics casts can never hit actors — so the blade
// segment (grip -> grip + bladeDir * bladeLength * hitTolerance) is
// tested directly against the actor capsule's axis segment: closest
// segment-segment distance < capsule radius.
bool segmentHitsCapsule(const Vec3& a0, const Vec3& a1, const Vec3& capA,
                        const Vec3& capB, f32 radius);

// A5 — directional blocking. If the attacker stands inside the
// defender's front cone (horizontal, blockAngleDegrees full width), the
// event's damage channels shrink by blockFactor and the blocked amount
// is rerouted to POSTURE (× blockPostureFactor) — running the guard down
// until the stagger breaks it (docs/STATS.md posture). Call it before
// applyDamage on a defender carrying State.Blocking; returns whether the
// guard caught the hit. Both camps go through this one helper.
bool applyBlock(DamageEvent& event, const Vec3& defenderFacing,
                const Vec3& defenderPos, const Vec3& attackerPos,
                f32 blockAngleDegrees, f32 blockFactor,
                f32 blockPostureFactor);

} // namespace gameplay
