#pragma once

#include "data/forms/Form.hpp"
#include "engine/core/Defines.hpp"
#include "gameplay/ability/DerivedStats.hpp"
#include "gameplay/stats/CharacterStats.hpp"
#include "gameplay/stats/Damage.hpp"
#include "gameplay/stats/Drugs.hpp"
#include "gameplay/stats/Injuries.hpp"
#include "gameplay/stats/Resonance.hpp"
#include "gameplay/stats/ResonanceDecays.hpp"
#include "gameplay/stats/StatsTuning.hpp"
#include "gameplay/stats/StatusBuildup.hpp"
#include "gameplay/stats/Survival.hpp"

// Architecture du passage du temps (docs/STATS.md §3, Phase 8).
//
// Two tick paths co-exist:
//   - Real-time path (update(), dt):  energy/posture regen, stagger/paralysis,
//     status buildup DoT + decay (tickBuildup stays in update()).
//   - Game-time path (tickGameTime, gameDt): health regen, essence regen, survival,
//     drug expiry, injury/affliction recovery (via GAS game-time effects).
//
// Time-skip (advanceGameTime): converts gameDt to real-time equivalents, runs in
// 10 s real-time chunks so death/status-expiry are detected mid-skip.

namespace gameplay {

// Full character state bundle for game-time functions.
// ActiveDrugs, Afflictions, and their FormDatabase are removed — all these now
// live as GAS activeEffects (gameTime=true) in the AbilitySystem.
struct GameTimeTickArgs {
    CoreAttributes& core;
    AttributeSet& vitals;
    AbilitySystem& system;
    CombatState& combat;
    StatusBuildup& buildup;
    Survival& survival;
    Injuries& injuries;         // for recoverInjuries + syncInjuryEffects
    Resonance& resonance;
    ResonanceDecays& resoDecays;
    const DerivedStatRegistry& derived;
    const GameplayTagRegistry& tags;
    const StatsTuningForm& tuning;
};

// Result of a time-skip (advanceGameTime).
struct GameTimeResult {
    bool died { false };
};

// Assembles the StatModifiers for a character: resonance cascade (from GAS
// current values after Phase A), equipment mods, and regen-rate suppression
// from active statuses. All resonance sources (survival, injuries, afflictions,
// drugs) are already in the GAS activeEffects; only the cascade and equipment
// remain as external contributions.
StatModifiers buildCharacterMods(GameTimeTickArgs& args,
                                 const StatModifiers& equipmentMods = {});

// Per-frame game-time tick: health regen, essence regen, survival effects,
// game-time GAS effects (drugs/afflictions), injury recovery, resonance decay,
// rest accrual. Does NOT tick status buildup.
// `mods` must already be computed and recomputeStats called by the caller.
void tickGameTime(GameTimeTickArgs& args, f64 gameDt, const StatModifiers& mods);

// Simulates `gameDt` game-seconds for a time-skip (Advance / Sleep buttons).
GameTimeResult advanceGameTime(GameTimeTickArgs& args, f64 gameDt, f32 timescale,
                               const StatModifiers& equipmentMods = {});

} // namespace gameplay
