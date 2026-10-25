#pragma once

#include "engine/core/Defines.hpp"

namespace core {
class Rng;
}

namespace gameplay {

struct StatsTuningForm;

// ---- Downed, bleedout, aggravation, convalescence ---------------------
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
// V1 rule (docs/FOLLOWERS-TEST.md): roll downedDeathChance for
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
