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
