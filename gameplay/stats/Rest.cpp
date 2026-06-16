#include "gameplay/stats/Rest.hpp"

#include <algorithm>

namespace gameplay {

namespace {
constexpr f32 kComfortableSleepHours = 8.0f; // a full night restores sleep fully
constexpr f32 kSleepPerHour = 2.0f;          // otherwise +2 sleep per hour slept
} // namespace

void accrueRest(CombatState& combat, f64 gameDt) {
    combat.restSeconds += static_cast<f32>(gameDt);
}

f64 sleep(GameClock& clock, Survival& survival, CombatState& combat, f32 hours,
          const StatsTuningForm& tuning) {
    const f64 gameDt = static_cast<f64>(hours) * 3600.0;
    const f32 sleepBefore = survival.sleep;

    clock.gameSeconds += gameDt;
    tickSurvival(survival, gameDt, tuning); // hunger/thirst decay while asleep...

    // ...but sleeping restores the sleep need rather than depleting it (§2).
    survival.sleep = hours >= kComfortableSleepHours
                         ? 100.0f
                         : std::min(100.0f, sleepBefore + kSleepPerHour * hours);

    combat.restSeconds += static_cast<f32>(gameDt); // sleeping counts as rest
    return gameDt;
}

} // namespace gameplay
