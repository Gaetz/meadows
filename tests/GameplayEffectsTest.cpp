#include <doctest/doctest.h>

#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/Attributes.hpp"
#include "gameplay/ability/GameplayEffects.hpp"

using namespace gameplay;

namespace {

struct Target {
    AttributeSet set;
    AbilitySystem system;
    GameplayTagRegistry registry;

    Target() { initializeCurrent(system, set); }
};

EffectForm instantOn(const char* attribute, const char* op, f32 magnitude) {
    EffectForm e;
    e.attribute = attribute;
    e.op = op;
    e.magnitude = magnitude;
    e.duration = "instant";
    return e;
}

} // namespace

TEST_CASE("effects: instant damage routes the meta-attribute into health") {
    Target t;
    CHECK(applyEffect(t.set, t.system, instantOn("damage", "add", 30.0f),
                      t.registry));
    CHECK(baseValueOf(t.set, attr("health")) == 70.0f);
    CHECK(baseValueOf(t.set, attr("damage")) == 0.0f); // meta reset
    CHECK(currentValueOf(t.system, attr("health")) == 70.0f);
}

TEST_CASE("effects: health is clamped to [0, maxHealth]") {
    Target t;
    applyEffect(t.set, t.system, instantOn("damage", "add", 500.0f), t.registry);
    CHECK(baseValueOf(t.set, attr("health")) == 0.0f); // not negative

    applyEffect(t.set, t.system, instantOn("health", "add", 999.0f), t.registry);
    CHECK(baseValueOf(t.set, attr("health")) == 100.0f); // capped at maxHealth
}

TEST_CASE("effects: an infinite modifier raises CurrentValue, not BaseValue") {
    Target t;
    EffectForm buff = instantOn("maxHealth", "add", 50.0f);
    buff.duration = "infinite";

    CHECK(applyEffect(t.set, t.system, buff, t.registry));
    CHECK(currentValueOf(t.system, attr("maxHealth")) == 150.0f);
    CHECK(baseValueOf(t.set, attr("maxHealth")) == 100.0f); // base untouched
}

TEST_CASE("effects: a duration modifier reverts on expiry and drops its tag") {
    Target t;
    t.registry.registerTag("Status.Burning");
    const GameplayTag burning = *t.registry.find("Status.Burning");

    EffectForm buff = instantOn("armorRating", "add", 20.0f);
    buff.duration = "duration";
    buff.durationSeconds = 2.0f;
    buff.grantedTag = "Status.Burning";

    applyEffect(t.set, t.system, buff, t.registry);
    CHECK(currentValueOf(t.system, attr("armorRating")) == 20.0f);
    CHECK(t.system.tags.has(burning));

    tickEffects(t.set, t.system, 1.0f, t.registry);
    CHECK(currentValueOf(t.system, attr("armorRating")) == 20.0f); // still active

    tickEffects(t.set, t.system, 1.5f, t.registry); // total 2.5s > 2s
    CHECK(currentValueOf(t.system, attr("armorRating")) == 0.0f); // reverted
    CHECK_FALSE(t.system.tags.has(burning));
    CHECK(t.system.activeEffects.empty());
}

TEST_CASE("effects: required and blocked tags gate application") {
    Target t;
    t.registry.registerTag("Immune.Fire");
    t.registry.registerTag("State.InCombat");

    // Immunity: a blocked tag the target owns rejects the effect.
    t.system.tags.add(*t.registry.find("Immune.Fire"), t.registry);
    EffectForm fire = instantOn("damage", "add", 10.0f);
    fire.blockedTag = "Immune.Fire";
    CHECK_FALSE(applyEffect(t.set, t.system, fire, t.registry));
    CHECK(baseValueOf(t.set, attr("health")) == 100.0f); // unaffected

    // Requirement: needs a tag the target lacks, then has.
    EffectForm combatBuff = instantOn("armorRating", "add", 5.0f);
    combatBuff.requiredTag = "State.InCombat";
    CHECK_FALSE(applyEffect(t.set, t.system, combatBuff, t.registry));
    t.system.tags.add(*t.registry.find("State.InCombat"), t.registry);
    CHECK(applyEffect(t.set, t.system, combatBuff, t.registry));
    CHECK(baseValueOf(t.set, attr("armorRating")) == 5.0f);
}

TEST_CASE("effects: a periodic effect ticks BaseValue then expires") {
    Target t;
    EffectForm poison = instantOn("damage", "add", 5.0f);
    poison.duration = "periodic";
    poison.period = 1.0f;
    poison.durationSeconds = 3.0f;

    applyEffect(t.set, t.system, poison, t.registry);
    CHECK(baseValueOf(t.set, attr("health")) == 100.0f); // no immediate tick

    tickEffects(t.set, t.system, 1.0f, t.registry);
    CHECK(baseValueOf(t.set, attr("health")) == 95.0f);
    tickEffects(t.set, t.system, 1.0f, t.registry);
    CHECK(baseValueOf(t.set, attr("health")) == 90.0f);
    tickEffects(t.set, t.system, 1.0f, t.registry);
    CHECK(baseValueOf(t.set, attr("health")) == 85.0f);
    CHECK(t.system.activeEffects.empty()); // 3s elapsed, expired

    tickEffects(t.set, t.system, 1.0f, t.registry);
    CHECK(baseValueOf(t.set, attr("health")) == 85.0f); // gone, no more ticks
}

// 8.11 — the EffectPanel's authoring lint.
TEST_CASE("effectWarnings flags the silent authoring mistakes") {
    gameplay::EffectForm effect; // defaults: add/instant, all empty
    // Default form: no attribute, no buildup, no tag -> does nothing.
    auto warnings = gameplay::effectWarnings(effect);
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].find("does nothing") != str::npos);

    effect.attribute = "health";
    CHECK(gameplay::effectWarnings(effect).empty());

    effect.duration = "periodic"; // period 0, no duration
    warnings = gameplay::effectWarnings(effect);
    CHECK(warnings.size() == 2); // never ticks + no duration
    effect.period = 0.5f;
    effect.durationSeconds = 3.0f;
    CHECK(gameplay::effectWarnings(effect).empty());

    effect.duration = "instant"; // duration fields now ignored
    warnings = gameplay::effectWarnings(effect);
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].find("instant ignores") != str::npos);
    effect.durationSeconds = 0.0f;
    effect.period = 0.0f;

    effect.buildupType = "poison"; // exclusive with attribute
    warnings = gameplay::effectWarnings(effect);
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].find("buildup") != str::npos);
    effect.attribute = "";
    CHECK(gameplay::effectWarnings(effect).empty());

    effect.buildupType = "";
    effect.attribute = "health";
    effect.expiryMode = "decay"; // resonance-only
    warnings = gameplay::effectWarnings(effect);
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].find("resonance") != str::npos);
    effect.attribute = "onyx";
    CHECK(gameplay::effectWarnings(effect).empty());

    effect.op = "divide"; // unknown keyword
    warnings = gameplay::effectWarnings(effect);
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].find("unknown op") != str::npos);
}
