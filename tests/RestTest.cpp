#include <doctest/doctest.h>

#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/GameplayTags.hpp"
#include "gameplay/stats/CharacterStats.hpp"
#include "gameplay/stats/CoreAttributes.hpp"
#include "gameplay/stats/Damage.hpp"
#include "gameplay/stats/GameClock.hpp"
#include "gameplay/stats/Rest.hpp"
#include "gameplay/stats/Survival.hpp"

using namespace gameplay;

TEST_CASE("rest: accrues over non-combat time") {
    CombatState combat;
    accrueRest(combat, 3600.0);
    CHECK(combat.restSeconds == doctest::Approx(3600.0f));
    accrueRest(combat, 1800.0);
    CHECK(combat.restSeconds == doctest::Approx(5400.0f));
}

TEST_CASE("rest: a full night's sleep restores sleep, decays hunger, advances time") {
    GameClock clock;
    Survival s;
    s.sleep = 40.0f;
    s.hunger = 100.0f;
    CombatState combat;

    const f64 dt = sleep(clock, s, combat, 8.0f);
    CHECK(dt == doctest::Approx(8.0 * 3600.0));
    CHECK(clock.gameHours() == doctest::Approx(8.0));
    CHECK(s.sleep == doctest::Approx(100.0f)); // full at 8h
    CHECK(s.hunger < 100.0f);                  // decayed while asleep
    CHECK(combat.restSeconds == doctest::Approx(8.0 * 3600.0));
}

TEST_CASE("rest: a short nap restores 2 sleep per hour") {
    GameClock clock;
    Survival s;
    s.sleep = 50.0f;
    CombatState combat;
    sleep(clock, s, combat, 3.0f);
    CHECK(s.sleep == doctest::Approx(56.0f)); // 50 + 2×3
}

TEST_CASE("rest: a hit interrupts accrued rest") {
    DerivedStatRegistry reg;
    registerCoreDerivedStats(reg);
    GameplayTagRegistry tags;
    tags.registerTag("State.Dead");
    tags.registerTag("State.Staggered");
    CoreAttributes core;
    AttributeSet vitals;
    vitals.health = 300.0f;
    AbilitySystem sys;
    CombatState combat;
    recomputeStats(core, vitals, sys, reg, nullptr);

    accrueRest(combat, 5.0 * 3600.0);
    REQUIRE(combat.restSeconds == doctest::Approx(5.0 * 3600.0));

    StatBlock block { core, vitals, sys, combat };
    applyDamage(block, DamageEvent { { { DamageType::Slash, 20.0f } }, 0.0f }, tags,
                reg);
    CHECK(combat.restSeconds == doctest::Approx(0.0f)); // reset by the hit
}
