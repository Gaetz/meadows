#include <doctest/doctest.h>

#include "gameplay/combat/CombatAi.hpp"
#include "world/ai/Perception.hpp" // alertTo (the B3 call-for-help intel)

// Chantier P0 B3 — the combat move decision (one flat sim-pure function)
// and the call-for-help intel handoff.

using gameplay::chooseCombatMove;
using gameplay::CombatMove;
using gameplay::CombatSituation;

TEST_CASE("the combat move follows range, cooldown, sight and courage") {
    CombatSituation s;
    s.attackRange = 1.7f;
    s.preferredRange = 3.0f;
    s.canSee = true;

    // In range, ready: strike.
    s.distance = 1.5f;
    CHECK(chooseCombatMove(s) == CombatMove::Strike);

    // In range but cooling: circle instead of standing still.
    s.cooldownSeconds = 1.0f;
    CHECK(chooseCombatMove(s) == CombatMove::Strafe);
    // Still cooling inside the preferred band: keep circling.
    s.distance = 2.6f;
    CHECK(chooseCombatMove(s) == CombatMove::Strafe);

    // Beyond the band: close in (cooling or not).
    s.distance = 6.0f;
    CHECK(chooseCombatMove(s) == CombatMove::Approach);
    s.cooldownSeconds = 0.0f;
    CHECK(chooseCombatMove(s) == CombatMove::Approach);

    // No sight: investigate, regardless of ranges.
    s.distance = 1.0f;
    s.canSee = false;
    CHECK(chooseCombatMove(s) == CombatMove::Approach);
    s.canSee = true;

    // A swing in flight plays out on the spot.
    s.swinging = true;
    s.distance = 5.0f;
    CHECK(chooseCombatMove(s) == CombatMove::Strike);
    s.swinging = false;

    // Courage 0.75: fights at 30% health, breaks below 25% — even
    // mid-strike-range, even blind.
    s.distance = 1.0f;
    s.healthFraction = 0.30f;
    CHECK(chooseCombatMove(s) == CombatMove::Strike);
    s.healthFraction = 0.20f;
    CHECK(chooseCombatMove(s) == CombatMove::Flee);
    // A braver soul (0.9) still fights at 20%.
    s.courage = 0.9f;
    CHECK(chooseCombatMove(s) == CombatMove::Strike);
}

TEST_CASE("a comrade's call is fresh intel: straight to Alert at the "
          "reported position") {
    world::Perception p;
    p.memorySeconds = 2.0f;
    CHECK(world::awareState(p) == world::AwareState::Calm);

    world::alertTo(p, { 12.0f, 0.0f, -4.0f });
    CHECK(world::awareState(p) == world::AwareState::Alert);
    CHECK(p.lastKnownPos.x == doctest::Approx(12.0f));

    // The report counts as a sighting: full memory before the hunt
    // degrades into a search.
    world::updatePerception(p, false, {}, 1.5f);
    CHECK(world::awareState(p) == world::AwareState::Alert);
    world::updatePerception(p, false, {}, 1.0f);
    CHECK(world::awareState(p) == world::AwareState::Searching);
}
