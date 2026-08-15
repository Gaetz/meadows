#include "gameplay/stats/Rest.hpp"

namespace gameplay {

void accrueRest(CombatState& combat, f64 gameDt) {
    combat.restSeconds += static_cast<f32>(gameDt);
}

} // namespace gameplay
