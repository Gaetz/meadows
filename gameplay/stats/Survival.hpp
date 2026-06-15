#pragma once

#include "engine/core/Defines.hpp"
#include "engine/reflect/Reflect.hpp"
#include "gameplay/stats/Resonance.hpp"

// Survival needs (docs/STATS.md §2 "survie"). Slice: hunger (and thirst, decayed)
// drive health Resonance once they drop below the threshold. A reflected component
// (serializes §5). Resonance is reached through the resonance path, never by
// setting attributes directly (§2.9).

namespace gameplay {

struct Survival {
    f32 hunger { 100.0f };
    f32 thirst { 100.0f };

    REFLECT_BEGIN(Survival, void)
        REFLECT_FIELD(hunger)
        REFLECT_FIELD(thirst)
    REFLECT_END()
};

// Decays hunger (1 point / 3 in-game hours) and thirst (1 / hour) by an in-game
// time delta (seconds — the value GameClock::advance returns). Clamps at 0.
void tickSurvival(Survival& survival, f64 gameDt);

// The onyx (health) Resonance contribution from hunger: 0 at/above the threshold
// (75), then linear down to an empty-stomach magnitude at hunger 0. Transient — a
// function of current hunger, restored by eating (not an accumulation).
f32 hungerResonance(const Survival& survival);

// The persistent Resonance with the transient survival contribution folded into
// onyx. Feed this to buildResonanceModifiers at recompute time.
Resonance effectiveResonance(const Resonance& persistent, const Survival& survival);

} // namespace gameplay
