#include "gameplay/combat/PlayerAction.hpp"

namespace gameplay {

PlayerAction decidePlayerAction(const PlayerActionInputs& in) {
    // A swing already in flight keeps playing — it outlasts everything
    // but death (the MeleeSwing machine finishes its arc even reeling).
    if (in.swingInFlight) {
        return PlayerAction::Swinging;
    }
    // STATS.md §4: staggered = can't act, parry or dodge. A bow draw in
    // flight is CUT by the controller (the arrow stays nocked).
    if (in.staggered) {
        return PlayerAction::Idle;
    }
    // RMB = raised guard, melee only (review 7b: a drawn bow gives no
    // shield guard — RMB with a ranged weapon does nothing).
    if (in.blockHeld && in.weaponDrawn && !in.rangedWeapon) {
        return PlayerAction::Blocking;
    }
    if (in.drawingBow) {
        return PlayerAction::DrawingBow;
    }
    // Transient, one frame: the burst itself and the State.Dodging
    // i-frames stay effect-driven (the Dodge ability's effects, §6).
    if (in.dodgeRequested) {
        return PlayerAction::Dodging;
    }
    return PlayerAction::Idle;
}

} // namespace gameplay
