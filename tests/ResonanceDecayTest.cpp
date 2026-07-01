#include <doctest/doctest.h>

#include "gameplay/ability/AbilitySystem.hpp" // attr()
#include "gameplay/stats/Resonance.hpp"
#include "gameplay/stats/ResonanceDecays.hpp"

using namespace gameplay;

TEST_CASE("resonance decay: addResonanceDecayToResonance folds entries into resonance") {
    ResonanceDecays decays;
    decays.list.push_back({ attr("amber"),  -30.0f, 1.0f, -30.0f });
    decays.list.push_back({ attr("onyx"),   -10.0f, 1.0f, -10.0f });
    decays.list.push_back({ attr("garnet"),   5.0f, 1.0f,   5.0f });

    Resonance res; // all zeros

    addResonanceDecayToResonance(decays, res);

    CHECK(res.amber  == doctest::Approx(-30.0f));
    CHECK(res.onyx   == doctest::Approx(-10.0f));
    CHECK(res.garnet == doctest::Approx(  5.0f));
}

TEST_CASE("resonance decay: positive remaining decays toward zero") {
    ResonanceDecays decays;
    decays.list.push_back({ attr("amber"), 20.0f, 4.0f, 20.0f }); // 4 pts/hour

    tickResonanceDecays(decays, 3.0f); // 3 game-hours → -12

    REQUIRE_FALSE(decays.list.empty());
    CHECK(decays.list[0].remaining == doctest::Approx(8.0f));
}

TEST_CASE("resonance decay: negative remaining decays toward zero") {
    ResonanceDecays decays;
    decays.list.push_back({ attr("amber"), -30.0f, 2.0f, -30.0f }); // 2 pts/hour

    tickResonanceDecays(decays, 5.0f); // 5 game-hours → +10

    REQUIRE_FALSE(decays.list.empty());
    CHECK(decays.list[0].remaining == doctest::Approx(-20.0f));
}

TEST_CASE("resonance decay: fully-decayed entry is removed from the list") {
    ResonanceDecays decays;
    decays.list.push_back({ attr("onyx"),   -5.0f, 10.0f, -5.0f }); // fast decay: done in 0.5h
    decays.list.push_back({ attr("garnet"), 40.0f,  1.0f, 40.0f }); // slow: 40h remaining

    tickResonanceDecays(decays, 1.0f); // 1 game-hour

    // First entry was -5, decays at 10/h → would go to +5, but clamps to 0 → removed (< 0.001)
    REQUIRE(decays.list.size() == 1u);
    CHECK(decays.list[0].attrId == attr("garnet"));
    CHECK(decays.list[0].remaining == doctest::Approx(39.0f));
}

TEST_CASE("resonance decay: multiple entries accumulate correctly into resonance") {
    ResonanceDecays decays;
    // Two expired drugs both left aftershocks on amber
    decays.list.push_back({ attr("amber"), -15.0f, 1.0f, -15.0f });
    decays.list.push_back({ attr("amber"), -10.0f, 1.0f, -10.0f });

    Resonance res;
    res.amber = 5.0f; // some base value already

    addResonanceDecayToResonance(decays, res);

    CHECK(res.amber == doctest::Approx(-20.0f)); // 5 - 15 - 10
}
