#pragma once

#include "engine/core/Defines.hpp"
#include "gameplay/stats/Damage.hpp"     // CombatState (holds restSeconds)
#include "gameplay/stats/GameClock.hpp"
#include "gameplay/stats/StatsTuning.hpp"
#include "gameplay/stats/Survival.hpp"

// Rest & sleep (docs/STATS.md §5/§2). "Rest" is in-game time without a hit; it is
// the precondition for injury and resonance recovery (which hook in here as those
// land in N2). Sleeping advances the clock, restores the sleep need, and accrues
// rest. Damage resets rest (gameplay/stats/Damage::applyDamage).

namespace gameplay {

// Accrues rest by an in-game time delta — call each tick while not taking damage.
void accrueRest(CombatState& combat, f64 gameDt);

// Sleeps for `hours` in-game: advances the clock, decays hunger/thirst over that
// time, restores the sleep need (full at 8h, else +2/hour, §2), and accrues rest.
// Returns the in-game seconds slept. Injury/resonance recovery will hook in here.
f64 sleep(GameClock& clock, Survival& survival, CombatState& combat, f32 hours,
          const StatsTuningForm& tuning = {});

} // namespace gameplay
