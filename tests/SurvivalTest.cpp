#include <doctest/doctest.h>

#include "gameplay/stats/GameClock.hpp"
#include "gameplay/stats/Resonance.hpp"
#include "gameplay/stats/Survival.hpp"

using namespace gameplay;

TEST_CASE("survival: hunger decays one point per three in-game hours") {
    Survival s;
    CHECK(s.hunger == doctest::Approx(100.0f));
    tickSurvival(s, 3.0 * 3600.0); // 3 in-game hours → -1
    CHECK(s.hunger == doctest::Approx(99.0f));
    tickSurvival(s, 72.0 * 3600.0); // 24 in-game points → 75
    CHECK(s.hunger == doctest::Approx(75.0f));
}

TEST_CASE("survival: hunger below 75 yields negative health resonance, linearly") {
    Survival s;
    s.hunger = 75.0f;
    CHECK(hungerResonance(s) == doctest::Approx(0.0f));
    s.hunger = 90.0f;
    CHECK(hungerResonance(s) == doctest::Approx(0.0f)); // above threshold: none
    s.hunger = 37.5f;
    CHECK(hungerResonance(s) == doctest::Approx(-25.0f)); // halfway → half of -50
    s.hunger = 0.0f;
    CHECK(hungerResonance(s) == doctest::Approx(-50.0f)); // empty stomach
}

TEST_CASE("survival: folds into the effective resonance (onyx), no direct set") {
    Survival s;
    s.hunger = 0.0f;
    Resonance persistent;
    persistent.onyx = -10.0f;
    const Resonance eff = effectiveResonance(persistent, s);
    CHECK(eff.onyx == doctest::Approx(-60.0f)); // -10 persistent + -50 hunger
    CHECK(eff.amber == doctest::Approx(0.0f));
}

TEST_CASE("survival: low sleep drives essence (garnet) resonance, hunger/thirst onyx") {
    Survival s;
    s.sleep = 0.0f; // hunger/thirst still full
    Resonance eff = effectiveResonance(Resonance {}, s);
    CHECK(eff.garnet == doctest::Approx(-50.0f));
    CHECK(eff.onyx == doctest::Approx(0.0f));

    s.hunger = 0.0f;
    s.thirst = 0.0f;
    eff = effectiveResonance(Resonance {}, s);
    CHECK(eff.onyx == doctest::Approx(-100.0f)); // hunger -50 + thirst -50 (cumulative)
}

TEST_CASE("survival: advancing the clock drives hunger below threshold → resonance") {
    GameClock clock; // timescale ×10
    Survival s;
    const f64 gameDt = clock.advance(30000.0f); // 30000 real s → 300000 game s ≈ 83h
    tickSurvival(s, gameDt);
    CHECK(s.hunger < 75.0f);
    CHECK(hungerResonance(s) < 0.0f);
}
