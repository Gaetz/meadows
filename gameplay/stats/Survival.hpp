#pragma once

#include "engine/core/Defines.hpp"
#include "engine/reflect/Reflect.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/GameplayEffects.hpp"
#include "gameplay/ability/GameplayTags.hpp"
#include "gameplay/stats/StatsTuning.hpp"

// Survival needs (docs/STATS.md §2 "survie"). Below a threshold each need drives
// Resonance via GAS effects: hunger + thirst → amber (energy), sleep → garnet (essence).
// updateSurvivalEffects() re-applies infinite GAS effects each game-time tick to keep
// magnitudes in sync with current need levels. A reflected component (serializes §5).

namespace gameplay {

struct AttributeSet; // from Attributes.hpp via GameplayEffects.hpp

struct Survival {
    f32 hunger { 100.0f };
    f32 thirst { 100.0f };
    f32 sleep { 100.0f };

    REFLECT_BEGIN(Survival, void)
        REFLECT_FIELD(hunger)
        REFLECT_FIELD(thirst)
        REFLECT_FIELD(sleep)
    REFLECT_END()
};

// Decays hunger / thirst / sleep by an in-game time delta (seconds), at the
// per-point rates in `tuning`. Clamps at 0.
void tickSurvival(Survival& survival, f64 gameDt, const StatsTuningForm& tuning = {});

// Recomputes the survival GAS effects on amber/garnet to match current need levels.
// Must be called after tickSurvival() so the effects are up to date for the next
// Phase A recompute. Uses infinite effects tagged "Internal.SurvivalAmber" /
// "Internal.SurvivalGarnet" (registered by registerSurvivalTags at startup).
void updateSurvivalEffects(Survival& survival, AbilitySystem& system,
                           AttributeSet& vitals, const GameplayTagRegistry& tags,
                           const StatsTuningForm& tuning = {});

// Pre-register the internal survival effect tags. Call once at startup
// (registerStatsFormTypes or registerGameplayComponents).
void registerSurvivalTags(GameplayTagRegistry& tags);

} // namespace gameplay
