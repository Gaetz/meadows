#include "gameplay/stats/Survival.hpp"

#include <algorithm>

namespace gameplay {

namespace {
// Tuning (would live in StatsTuningForm, §5; defaults here for the slice).
constexpr f32 kThreshold = 75.0f;          // below this, a need bites
constexpr f32 kResonanceAtEmpty = -50.0f;  // resonance contribution at value 0
constexpr f64 kHungerHoursPerPoint = 3.0;  // -1 hunger per 3 in-game hours
constexpr f64 kThirstHoursPerPoint = 1.0;  // -1 thirst per in-game hour
constexpr f64 kSleepHoursPerPoint = 1.0;   // -1 sleep per in-game hour

// Shared linear contribution: 0 at/above the threshold, down to the empty
// magnitude at 0. Transient (a function of the current value, not accumulated).
f32 needResonance(f32 value) {
    if (value >= kThreshold) {
        return 0.0f;
    }
    return (kThreshold - value) / kThreshold * kResonanceAtEmpty; // 0 .. empty
}
} // namespace

void tickSurvival(Survival& survival, f64 gameDt) {
    const f64 hours = gameDt / 3600.0;
    survival.hunger = std::max(
        0.0f, survival.hunger - static_cast<f32>(hours / kHungerHoursPerPoint));
    survival.thirst = std::max(
        0.0f, survival.thirst - static_cast<f32>(hours / kThirstHoursPerPoint));
    survival.sleep = std::max(
        0.0f, survival.sleep - static_cast<f32>(hours / kSleepHoursPerPoint));
}

f32 hungerResonance(const Survival& survival) {
    return needResonance(survival.hunger);
}

Resonance effectiveResonance(const Resonance& persistent, const Survival& survival) {
    Resonance r = persistent;
    r.onyx += needResonance(survival.hunger) + needResonance(survival.thirst);
    r.garnet += needResonance(survival.sleep);
    return r;
}

} // namespace gameplay
