#pragma once

#include "engine/core/Defines.hpp"

// The player's ACTION arbiter, the sim-pure half: WHICH
// exclusive action the frame gets. Same pattern as decideMoveMode:
// the state is an enum and ONE flat function owns every
// transition/exclusion — guard vs swing, stagger, bow draw, dodge — and
// the controller only executes (guard clock, viewmodel, burst, effects).

namespace gameplay {

enum class PlayerAction : u8 { Idle, Swinging, Blocking, DrawingBow,
                               Dodging };

// The frame's raw facts, gathered by the controller. All booleans on
// purpose: the arbiter decides, it never reads components or input.
struct PlayerActionInputs {
    bool weaponDrawn { false };   // R-toggle state
    bool rangedWeapon { false };  // equipped weapon has projectileSpeed > 0
    bool staggered { false };     // State.Staggered (§4: can't act/parry/dodge)
    bool blockHeld { false };     // RMB down
    bool swingInFlight { false }; // MeleeSwing.phase != Idle
    bool drawingBow { false };    // a charge is in flight (bowCharge >= 0)
    bool dodgeRequested { false };// sprint-key tap detected this frame
};

// THE decision (one function owns every transition). Priority:
// a swing in flight outlasts everything; a stagger cuts everything else
// (the controller releases a cut bow draw); then guard, draw, dodge.
// A ranged weapon raises NO guard — RMB does nothing with a bow in hand.
PlayerAction decidePlayerAction(const PlayerActionInputs& in);

} // namespace gameplay
