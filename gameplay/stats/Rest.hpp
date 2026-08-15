#pragma once

#include "engine/core/Defines.hpp"
#include "gameplay/stats/Damage.hpp" // CombatState (holds restSeconds)

// Rest (docs/STATS.md §5/§2): in-game time without a hit — the
// precondition for injury and resonance recovery. Damage resets it
// (gameplay/stats/Damage::applyDamage). Sleeping in a bed goes through
// gameplay::sleepGameTime (GameTime.hpp), which credits the night as rest
// and runs the time-skip systems over it.

namespace gameplay {

// Accrues rest by an in-game time delta — call each tick while not taking damage.
void accrueRest(CombatState& combat, f64 gameDt);

} // namespace gameplay
