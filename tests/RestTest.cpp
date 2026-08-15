#include <doctest/doctest.h>

#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/GameplayTags.hpp"
#include "gameplay/stats/CharacterStats.hpp"
#include "gameplay/stats/CoreAttributes.hpp"
#include "gameplay/stats/Damage.hpp"
#include "gameplay/stats/GameClock.hpp"
#include "gameplay/stats/GameTime.hpp"
#include "gameplay/stats/Injuries.hpp"
#include "gameplay/stats/Rest.hpp"
#include "gameplay/stats/Survival.hpp"

using namespace gameplay;

namespace {

// Full character bundle for the time-skip sleep (the args struct wants
// every component the game-time path can touch).
struct SleepFixture {
    DerivedStatRegistry reg;
    GameplayTagRegistry tags;
    StatsTuningForm tuning;
    GameClock clock;
    CoreAttributes core;
    AttributeSet vitals;
    AbilitySystem sys;
    CombatState combat;
    StatusBuildup buildup;
    Survival survival;
    Injuries injuries;
    Resonance resonance;
    ResonanceDecays decays;

    SleepFixture() {
        registerCoreDerivedStats(reg);
        tags.registerTag("State.Dead");
        tags.registerTag("State.Staggered");
        tags.registerTag("State.Paralyzed");
        recomputeStats(core, vitals, resonance, sys, reg, nullptr);
        vitals.health = currentValueOf(sys, attr("maxHealth"));
    }

    GameTimeTickArgs args() {
        return { core,     vitals,    sys,    combat, buildup, survival,
                 injuries, resonance, decays, reg,    tags,    tuning };
    }
};

} // namespace

TEST_CASE("rest: accrues over non-combat time") {
    CombatState combat;
    accrueRest(combat, 3600.0);
    CHECK(combat.restSeconds == doctest::Approx(3600.0f));
    accrueRest(combat, 1800.0);
    CHECK(combat.restSeconds == doctest::Approx(5400.0f));
}

TEST_CASE("sleep: a full night restores sleep, decays hunger, advances time") {
    SleepFixture f;
    f.survival.sleep = 40.0f;
    f.survival.hunger = 100.0f;

    GameTimeTickArgs a = f.args();
    sleepGameTime(f.clock, a, 8.0f);
    CHECK(f.clock.gameHours() == doctest::Approx(8.0));
    CHECK(f.survival.sleep == doctest::Approx(100.0f)); // full at 8h
    CHECK(f.survival.hunger < 100.0f);                  // decayed while asleep
    CHECK(f.combat.restSeconds >= doctest::Approx(8.0 * 3600.0));
}

TEST_CASE("sleep: a short nap restores 2 sleep per hour") {
    SleepFixture f;
    f.survival.sleep = 50.0f;
    GameTimeTickArgs a = f.args();
    sleepGameTime(f.clock, a, 3.0f);
    CHECK(f.survival.sleep == doctest::Approx(56.0f)); // 50 + 2×3
}

TEST_CASE("sleep: health regenerates over the slept window") {
    SleepFixture f;
    const f32 maxHealth = currentValueOf(f.sys, attr("maxHealth"));
    REQUIRE(maxHealth > 0.0f);
    f.vitals.health = maxHealth * 0.25f;

    GameTimeTickArgs a = f.args();
    sleepGameTime(f.clock, a, 8.0f);
    // The whole night runs through the game-time regen (the bed bug: the
    // clock jump used to bypass tickGameTime entirely).
    CHECK(f.vitals.health > maxHealth * 0.25f);
}

TEST_CASE("sleep: injuries recover even right after a fight") {
    SleepFixture f;
    f.combat.restSeconds = 0.0f; // just hit — no rest banked yet
    addInjury(f.injuries, InjuryType::Cut, BodyPart::Torso);
    REQUIRE(f.injuries.list.size() == 1);
    const f32 before = f.injuries.list[0].recoveryHoursRemaining;

    GameTimeTickArgs a = f.args();
    sleepGameTime(f.clock, a, 8.0f);
    // The night is credited as rest BEFORE the skip, so recovery runs.
    CHECK(f.injuries.list[0].recoveryHoursRemaining < before);
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

TEST_CASE("wait: game-time effects expire and needs decay — no sleep restore") {
    SleepFixture f;
    EffectForm drug;
    drug.attribute = "amber";
    drug.op = "add";
    drug.magnitude = 50.0f;
    drug.durationHours = 4.0f;
    REQUIRE(applyEffect(f.vitals, f.sys, drug, f.tags));
    REQUIRE(f.sys.activeEffects.size() == 1);

    GameTimeTickArgs a = f.args();
    waitGameTime(f.clock, a, 6.0f);
    CHECK(f.clock.gameHours() == doctest::Approx(6.0));
    CHECK(f.sys.activeEffects.empty());     // the 4h drug wore off waiting
    CHECK(f.survival.sleep < 100.0f);       // waiting is not sleeping
    CHECK(f.combat.restSeconds >= doctest::Approx(6.0 * 3600.0));
}
