#pragma once

#include "engine/core/Defines.hpp"
#include "engine/reflect/Reflect.hpp"
#include "gameplay/stats/Resonance.hpp"

// Survival needs (docs/STATS.md §2 "survie"). Below a threshold each need drives
// Resonance: hunger + thirst → onyx (health), sleep → garnet (essence). A reflected
// component (serializes §5). Resonance is reached through the resonance path, never
// by setting attributes directly (§2.9). (Temperature → amber is Phase 4.7.)

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

// Decays hunger (1 point / 3 in-game hours), thirst and sleep (1 / hour each) by
// an in-game time delta (seconds — the value GameClock::advance returns). Clamp 0.
void tickSurvival(Survival& survival, f64 gameDt);

// The onyx (health) Resonance contribution from hunger alone: 0 at/above the
// threshold (75), then linear to an empty magnitude at 0. Transient — a function
// of current hunger, restored by eating (not an accumulation).
f32 hungerResonance(const Survival& survival);

// The persistent Resonance with the transient survival contributions folded in:
// hunger + thirst → onyx, sleep → garnet. Feed this to buildResonanceModifiers.
Resonance effectiveResonance(const Resonance& persistent, const Survival& survival);

} // namespace gameplay
