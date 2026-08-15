#include <doctest/doctest.h>
#include "gameplay/combat/PlayerAction.hpp"

using gameplay::decidePlayerAction;
using gameplay::PlayerAction;
using gameplay::PlayerActionInputs;

TEST_CASE("player action: plain frames are Idle") {
    // Nothing pressed, nothing in flight: Idle, drawn or sheathed.
    PlayerActionInputs in;
    CHECK(decidePlayerAction(in) == PlayerAction::Idle);
    in.weaponDrawn = true;
    CHECK(decidePlayerAction(in) == PlayerAction::Idle);
}

TEST_CASE("player action: the guard needs a DRAWN melee weapon") {
    // RMB with the weapon sheathed raises nothing...
    PlayerActionInputs in;
    in.blockHeld = true;
    CHECK(decidePlayerAction(in) == PlayerAction::Idle);
    // ...drawn, it is the raised guard.
    in.weaponDrawn = true;
    CHECK(decidePlayerAction(in) == PlayerAction::Blocking);
}

TEST_CASE("player action: a bow raises no guard") {
    // RMB with a ranged weapon grants no guard — a bow is not a
    // shield.
    PlayerActionInputs in;
    in.weaponDrawn = true;
    in.rangedWeapon = true;
    in.blockHeld = true;
    CHECK(decidePlayerAction(in) == PlayerAction::Idle);
    // A charge in flight stays a draw even with RMB held.
    in.drawingBow = true;
    CHECK(decidePlayerAction(in) == PlayerAction::DrawingBow);
}

TEST_CASE("player action: a stagger drops the guard, cuts the draw and "
          "refuses the dodge (STATS.md §4)") {
    PlayerActionInputs in;
    in.weaponDrawn = true;
    in.staggered = true;
    in.blockHeld = true;
    CHECK(decidePlayerAction(in) == PlayerAction::Idle); // guard drops
    in.blockHeld = false;
    in.dodgeRequested = true;
    CHECK(decidePlayerAction(in) == PlayerAction::Idle); // no dodge
    in.dodgeRequested = false;
    in.drawingBow = true;
    in.rangedWeapon = true;
    // Idle while a charge is up = the controller releases it (the
    // arrow stays nocked).
    CHECK(decidePlayerAction(in) == PlayerAction::Idle);
}

TEST_CASE("player action: a swing in flight outlasts everything") {
    // The guard waits for the swing to land...
    PlayerActionInputs in;
    in.weaponDrawn = true;
    in.swingInFlight = true;
    in.blockHeld = true;
    CHECK(decidePlayerAction(in) == PlayerAction::Swinging);
    // ...a stagger doesn't cancel the arc already travelling...
    in.blockHeld = false;
    in.staggered = true;
    CHECK(decidePlayerAction(in) == PlayerAction::Swinging);
    // ...and a dodge tap mid-swing is refused.
    in.staggered = false;
    in.dodgeRequested = true;
    CHECK(decidePlayerAction(in) == PlayerAction::Swinging);
}

TEST_CASE("player action: the dodge tap wins an otherwise idle frame") {
    PlayerActionInputs in;
    in.weaponDrawn = true;
    in.dodgeRequested = true;
    CHECK(decidePlayerAction(in) == PlayerAction::Dodging);
    // Exclusive actions: an in-flight draw keeps the frame.
    in.drawingBow = true;
    in.rangedWeapon = true;
    CHECK(decidePlayerAction(in) == PlayerAction::DrawingBow);
}
