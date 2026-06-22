#pragma once

#include "data/forms/Form.hpp"
#include "engine/core/Defines.hpp"
#include "gameplay/ability/DerivedStats.hpp"
#include "gameplay/stats/Afflictions.hpp"
#include "gameplay/stats/CharacterStats.hpp"
#include "gameplay/stats/Damage.hpp"
#include "gameplay/stats/Drugs.hpp"
#include "gameplay/stats/Injuries.hpp"
#include "gameplay/stats/Resonance.hpp"
#include "gameplay/stats/StatsTuning.hpp"
#include "gameplay/stats/StatusBuildup.hpp"
#include "gameplay/stats/Survival.hpp"

// Architecture du passage du temps (docs/STATS.md §3, Phase 8).
//
// Two tick paths co-exist:
//   - Real-time path (update(), dt):  energy/posture regen, stagger/paralysis,
//     status buildup DoT + decay (tickBuildup stays in update()).
//   - Game-time path (tickGameTime, gameDt): health regen, essence regen, survival,
//     drugs, injury/affliction recovery.
//
// Time-skip (advanceGameTime): converts gameDt to real-time equivalents, runs in
// 10 s real-time chunks so death/status-expiry are detected mid-skip.

namespace gameplay {

// Full character state bundle for game-time functions.
struct GameTimeTickArgs {
    CoreAttributes& core;
    AttributeSet& vitals;
    AbilitySystem& system;
    CombatState& combat;
    StatusBuildup& buildup;
    Survival& survival;
    ActiveDrugs& activeDrugs;
    Injuries& injuries;
    Afflictions& afflictions;
    Resonance& resonance;
    const data::FormDatabase& afflictionDb;
    const DerivedStatRegistry& derived;
    const GameplayTagRegistry& tags;
    const StatsTuningForm& tuning;
};

// Result of a time-skip (advanceGameTime).
struct GameTimeResult {
    bool died { false };
};

// Assembles the full StatModifiers for a character: resonance cascade, injuries,
// afflictions, drugs, equipment mods, and regen-rate suppression from active
// statuses (buildupStatusModifiers). Both tickCharacter and advanceGameTime use
// this as the single source of truth for modifier assembly.
StatModifiers buildCharacterMods(GameTimeTickArgs& args,
                                 const StatModifiers& equipmentMods = {});

// Per-frame game-time tick: health regen, essence regen, survival, drugs,
// injury/affliction recovery, rest accrual. Does NOT tick status buildup.
// `mods` must already be computed and recomputeStats called by the caller.
void tickGameTime(GameTimeTickArgs& args, f64 gameDt, const StatModifiers& mods);

// Simulates `gameDt` game-seconds for a time-skip (Advance / Sleep buttons).
// Converts to real-time equivalents and steps in 10 s real-time chunks, ticking
// status buildup (real-time DoT + decay) each chunk, then game-time effects.
// Stops early and returns died=true if health reaches 0.
// `equipmentMods` are equipment/fixed modifiers; resonance/injury/drug mods are
// recomputed internally each chunk.
GameTimeResult advanceGameTime(GameTimeTickArgs& args, f64 gameDt, f32 timescale,
                               const StatModifiers& equipmentMods = {});

} // namespace gameplay
