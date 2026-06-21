#pragma once

#include "engine/core/Defines.hpp"
#include "engine/reflect/Reflect.hpp"
#include "gameplay/stats/Resonance.hpp"
#include "gameplay/stats/StatsTuning.hpp"

// Survival needs (docs/STATS.md §2 "survie"). Below a threshold each need drives
// Resonance: hunger + thirst → amber (energy), sleep → garnet (essence). A reflected
// component (serializes §5). Resonance is reached through the resonance path, never
// by setting attributes directly (§2.9). (Temperature → onyx (health) is Phase 7.)

namespace gameplay {

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

// Decays hunger / thirst / sleep by an in-game time delta (seconds — the value
// GameClock::advance returns), at the per-point rates in `tuning`. Clamps at 0.
void tickSurvival(Survival& survival, f64 gameDt, const StatsTuningForm& tuning = {});

// The amber (energy) Resonance contribution from hunger alone: 0 at/above the
// threshold, then linear to the empty magnitude at 0 (both from `tuning`).
// Transient — a function of current hunger, restored by eating.
f32 hungerResonance(const Survival& survival, const StatsTuningForm& tuning = {});

// The persistent Resonance with the transient survival contributions folded in:
// hunger + thirst → amber (energy), sleep → garnet (essence). Feed this to buildResonanceModifiers.
Resonance effectiveResonance(const Resonance& persistent, const Survival& survival,
                             const StatsTuningForm& tuning = {});

} // namespace gameplay
