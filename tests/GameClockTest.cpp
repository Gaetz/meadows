#include <doctest/doctest.h>

#include "gameplay/stats/GameClock.hpp"

using namespace gameplay;

TEST_CASE("game clock: advance accumulates game time at the timescale") {
    GameClock clock; // timescale ×10
    CHECK(clock.advance(1.0f) == doctest::Approx(10.0)); // 1 real s → 10 game s
    CHECK(clock.gameSeconds == doctest::Approx(10.0));
    clock.advance(0.5f);
    CHECK(clock.gameSeconds == doctest::Approx(15.0));
}

TEST_CASE("game clock: hours and days derive from game seconds") {
    GameClock clock;
    clock.timescale = 1.0f;
    clock.advance(3600.0f); // one game hour
    CHECK(clock.gameHours() == doctest::Approx(1.0));
    clock.advance(3600.0f * 23.0f); // +23h → 24h
    CHECK(clock.gameDays() == doctest::Approx(1.0));
}

// --- The clock -> game-time-effects seam ---------------------------------------------
// tickGameTimeEffects itself is covered by AfflictionsTest/DrugsTest; what
// this locks is the CHAIN: advance() converts real dt at the timescale, that
// gameDt drives game-time expiry, and the two tick paths stay watertight
// (a game-time effect must never expire on REAL seconds and vice versa).

#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/GameplayEffects.hpp"
#include "gameplay/ability/GameplayTags.hpp"

TEST_CASE("game clock: advance() drives game-time effect expiry at timescale") {
    gameplay::GameClock clock;
    clock.timescale = 12.0f;

    gameplay::GameplayTagRegistry tags;
    gameplay::AttributeSet vitals;
    gameplay::AbilitySystem sys;
    gameplay::initializeCurrent(sys, vitals);

    gameplay::EffectForm drug;
    drug.attribute = "energy";
    drug.op = "add";
    drug.magnitude = 10.0f;
    drug.durationHours = 1.0f; // one GAME hour
    REQUIRE(gameplay::applyEffect(vitals, sys, drug, tags));
    REQUIRE(sys.activeEffects.size() == 1);

    // 200 real seconds at x12 = 2400 game seconds: still active.
    const f64 gameDt1 = clock.advance(200.0f);
    CHECK(gameDt1 == doctest::Approx(2400.0));
    gameplay::tickGameTimeEffects(vitals, sys, gameDt1, tags);
    CHECK(sys.activeEffects.size() == 1);

    // A REAL-time tick of the same magnitude must not touch it (watertight).
    gameplay::tickEffects(vitals, sys, 2400.0f, tags);
    CHECK(sys.activeEffects.size() == 1);

    // 150 more real seconds = 1800 game seconds -> 4200 total >= 3600: expires.
    const f64 gameDt2 = clock.advance(150.0f);
    gameplay::tickGameTimeEffects(vitals, sys, gameDt2, tags);
    CHECK(sys.activeEffects.empty());
    CHECK(clock.gameHours() == doctest::Approx(4200.0 / 3600.0));
}
