#include "gameplay/stats/Survival.hpp"

#include <algorithm>

namespace gameplay {

namespace {
// Shared linear contribution: 0 at/above the threshold, down to the empty
// magnitude at 0. Transient (a function of the current value, not accumulated).
f32 needResonance(f32 value, const StatsTuningForm& t) {
    if (value >= t.survivalThreshold) {
        return 0.0f;
    }
    return (t.survivalThreshold - value) / t.survivalThreshold *
           t.survivalResonanceAtEmpty; // 0 .. empty
}
} // namespace

void tickSurvival(Survival& survival, f64 gameDt, const StatsTuningForm& tuning) {
    const f64 hours = gameDt / 3600.0;
    survival.hunger = std::max(
        0.0f, survival.hunger - static_cast<f32>(hours / tuning.hungerHoursPerPoint));
    survival.thirst = std::max(
        0.0f, survival.thirst - static_cast<f32>(hours / tuning.thirstHoursPerPoint));
    survival.sleep = std::max(
        0.0f, survival.sleep - static_cast<f32>(hours / tuning.sleepHoursPerPoint));
}

f32 hungerResonance(const Survival& survival, const StatsTuningForm& tuning) {
    return needResonance(survival.hunger, tuning);
}

Resonance effectiveResonance(const Resonance& persistent, const Survival& survival,
                             const StatsTuningForm& tuning) {
    Resonance r = persistent;
    r.onyx += needResonance(survival.hunger, tuning) +
              needResonance(survival.thirst, tuning);
    r.garnet += needResonance(survival.sleep, tuning);
    return r;
}

} // namespace gameplay
