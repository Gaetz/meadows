#include "gameplay/stats/Survival.hpp"

#include <algorithm>

namespace gameplay {

namespace {
// Tuning (would live in StatsTuningForm, §5; defaults here for the slice).
constexpr f32 kThreshold = 75.0f;               // below this, hunger/thirst bite
constexpr f32 kHungerResonanceAtEmpty = -50.0f; // onyx at hunger 0
constexpr f64 kHungerHoursPerPoint = 3.0;       // -1 hunger per 3 in-game hours
constexpr f64 kThirstHoursPerPoint = 1.0;       // -1 thirst per in-game hour
} // namespace

void tickSurvival(Survival& survival, f64 gameDt) {
    const f64 hours = gameDt / 3600.0;
    survival.hunger = std::max(
        0.0f, survival.hunger - static_cast<f32>(hours / kHungerHoursPerPoint));
    survival.thirst = std::max(
        0.0f, survival.thirst - static_cast<f32>(hours / kThirstHoursPerPoint));
}

f32 hungerResonance(const Survival& survival) {
    if (survival.hunger >= kThreshold) {
        return 0.0f;
    }
    const f32 below = (kThreshold - survival.hunger) / kThreshold; // 0..1
    return below * kHungerResonanceAtEmpty;                        // 0 .. empty
}

Resonance effectiveResonance(const Resonance& persistent, const Survival& survival) {
    Resonance r = persistent;
    r.onyx += hungerResonance(survival); // hunger drives health resonance (§2)
    // Thirst follows the same pattern (also onyx, cumulative) — deferred to 4.7.
    return r;
}

} // namespace gameplay
