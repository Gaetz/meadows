#pragma once

#include <glm/glm.hpp> // Vec3 by value (Defines only forward-declares glm)

#include "engine/core/Defines.hpp"

// The follow decision (FOLLOWERS É1 — docs/CHANTIER-FOLLOWERS.md). Pure and
// headless (§2.10): position in, intent out — the game-side AI package
// (NpcScheduleController::followPlayer) executes the intent through the
// existing goTo/moveNpcAlongPath idiom. All feel knobs live in
// StatsTuningForm (§5 moddable), pulled through followTuning below.

namespace core {
class Rng;
}

namespace gameplay {

struct StatsTuningForm;

// The follow feel knobs, mirrored from StatsTuningForm (follow* fields).
struct FollowTuning {
    f32 nearRadius { 3.5f };     // closer than this: stand, face the player
    f32 catchupRadius { 8.0f };  // beyond this: hurry (catchupSpeed)
    f32 catchupSpeed { 1.25f };  // speedScale while catching up
    f32 teleportRadius { 40.0f };// beyond this: reposition next to the player
};

FollowTuning followTuning(const StatsTuningForm& tuning);

// What the follower should do this frame.
struct FollowIntent {
    bool move { false };      // walk toward `target`
    Vec3 target { 0.0f };     // the player's position (the path goal)
    f32 speedScale { 1.0f };  // moveNpcAlongPath scale (catchupSpeed when far)
    bool teleport { false };  // lost him: reposition near the player
};

// Distance is HORIZONTAL (both actors are terrain-grounded; the Y gap is
// presentation, not separation). Bands, inclusive at the low edge:
//   d <= near                : idle (move = false)
//   near < d <= catchup      : walk (speedScale 1)
//   catchup < d <= teleport  : walk fast (speedScale = catchupSpeed)
//   d > teleport             : teleport
FollowIntent decideFollow(const Vec3& followerPos, const Vec3& playerPos,
                          const FollowTuning& tuning);

// ---- É2: the aggro table --------------------------------------------------
// Pure per-event decision for OnHitTaken{source, target} (§2.11: the bus
// already carries the signal — resolveMeleeStrike dispatches it; this is
// only the reaction table). The caller resolves the ROLES (follower =
// FollowerState.followerActive, hostile = Npc.hostile, friendlyTrial =
// the Combat.FriendlyTrial tag on the PLAYER) and writes the returned
// entity into Npc.combatTarget. The rules, flat:
//   - a hostile struck BY A FOLLOWER fights that follower back (struck
//     by the player it keeps its DEFAULT player targeting — iso É2);
//   - a follower struck by a hostile re-aims at its attacker, even
//     mid-fight;
//   - a follower with no live target defends the party: it adopts a
//     hostile that hit the player or a fellow follower, and adopts the
//     hostile the PLAYER strikes first;
//   - Combat.FriendlyTrial suppresses every follower adoption (the
//     doc's brawl case); hostile retaliation is never suppressed;
//   - nobody ever targets itself, and the player is never adopted.
struct AggroRoles {
    u64 self { 0 };
    bool selfFollower { false };
    bool selfHostile { false };
    bool selfHasLiveTarget { false };
    bool sourcePlayer { false };
    bool sourceFollower { false };
    bool sourceHostile { false };
    bool targetPlayer { false };
    bool targetFollower { false };
    bool targetHostile { false };
    bool friendlyTrial { false };
};

// The entity `self` should adopt as its combat target (the event's
// source or target id), or 0 = keep the current one.
u64 adoptOnHit(u64 source, u64 target, const AggroRoles& roles);

// OnDeath{target}: should a combat target pointing at `dead` be cleared?
// (A cleared follower falls back to the follow package next frame.)
bool disengageOnDeath(u64 dead, u64 combatTarget);

// ---- É3: downed, bleedout, aggravation, convalescence ---------------------
// The rules live HERE (sim, headless, doctested); the game side
// (FollowerController::updateDowned) only sweeps the live NPCs and calls
// in. Reused systems (§2.11): the CombatState timer pattern (stagger),
// the Injuries container + syncInjuryEffects, the seeded core::Rng (§8),
// StatsTuningForm knobs (§5), and updateLifeState as the ONE life-state
// write point.

struct Injury;
struct Injuries;
struct StatBlock;
struct FollowerState;
class GameplayTagRegistry;

// A wound on an already-wounded body can turn ugly (docs/FOLLOWERS.md §2:
// death is aggravation-based, never the first fall). Rolled on the seeded
// engine RNG; `alreadyInjured == false` never aggravates AND draws
// nothing (the random stream stays untouched — determinism stays legible).
// Order: the death roll first, then the worse-injury roll.
enum class Aggravation : u8 { None, WorseInjury, Death };
Aggravation rollAggravation(bool alreadyInjured, core::Rng& rng,
                            const StatsTuningForm& tuning);

// The end of an un-revived bleedout window (updateDowned returned true).
// V1 rule (stated in docs/FOLLOWERS-TEST É3): roll downedDeathChance for
// a REAL death — the protection tag is lifted and updateLifeState (the
// single write point) grants State.Dead, so the normal OnDeath flow runs.
// Otherwise he gets back up at 1 HP (a §2.9 execution calculation: the
// BaseValue write + recompute, the applyBuildupResult idiom) with a fresh
// torso cut; if he was ALREADY wounded, rollAggravation may worsen the
// wound (severity +1) — or kill after all.
enum class BleedoutOutcome : u8 { Died, Recovered };
struct BleedoutResult {
    BleedoutOutcome outcome { BleedoutOutcome::Recovered };
    bool aggravated { false }; // the fresh wound landed on a wounded body
};
BleedoutResult resolveBleedout(StatBlock& target, Injuries& injuries,
                               core::Rng& rng,
                               const GameplayTagRegistry& tags,
                               const StatsTuningForm& tuning);

// Convalescence rules: a follower whose injuries crossed the severity bar
// (any injury at severity >= 1 — i.e. wounded AGAIN before healing)
// demands rest at home. `convalescenceHours` = the longest remaining
// recovery among his injuries (the Injury.recoveryHoursRemaining clock —
// the injuries system's own timer, not a new one).
bool needsConvalescence(const Injuries& injuries);
f32 convalescenceHours(const Injuries& injuries);

// True while `state.followerDownedRecoveryHours` (an absolute GameClock
// game-hour stamp — the VendorState.lastRestockHours idiom) lies in the
// future: recruiting is refused, mirrored as the player's
// Follower.Convalescent tag for the dialogue conditions.
bool followerConvalescent(const FollowerState& state, f64 nowHours);

} // namespace gameplay
