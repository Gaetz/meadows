#include <doctest/doctest.h>

#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/GameplayTags.hpp"
#include "gameplay/stats/CharacterStats.hpp"
#include "gameplay/stats/CoreAttributes.hpp"
#include "gameplay/stats/Damage.hpp"
#include "gameplay/stats/StatsTuning.hpp"

using namespace gameplay;

// Offensive derived stats (attack, criticalDamage,
// armor/resist penetration) and their consumption in the damage pipeline.

namespace {
struct Fixture {
    CoreAttributes core;
    AttributeSet vitals;
    AbilitySystem system;
    CombatState combat;
    DerivedStatRegistry derived;
    GameplayTagRegistry tags;

    Fixture() {
        core.strength = core.constitution = core.grace = 20.0f;
        core.dexterity = core.alacrity = core.perception = 20.0f;
        core.charisma = core.ego = core.insight = 20.0f;
        vitals.health = 300.0f;
        registerCoreDerivedStats(derived);
        tags.registerTag("State.Dead");
        tags.registerTag("State.Staggered");
        recomputeStats(core, vitals, system, derived, nullptr);
        combat.posture = currentValueOf(system, attr("maxPosture"));
    }
    StatBlock block() { return StatBlock { core, vitals, system, combat }; }
};
} // namespace

TEST_CASE("offensive stats: derived formulas (docs/STATS.md §3)") {
    Fixture f;
    // attack = 5 + strength = 25; criticalDamage = 1.5 + 20·0.05 = 2.5;
    // pens = (attr − 5)·0.5 = 7.5.
    CHECK(currentValueOf(f.system, attr("attack")) == doctest::Approx(25.0f));
    CHECK(currentValueOf(f.system, attr("criticalDamage")) ==
          doctest::Approx(2.5f));
    CHECK(currentValueOf(f.system, attr("armorPenetration")) ==
          doctest::Approx(7.5f));
    CHECK(currentValueOf(f.system, attr("resistPenetration")) ==
          doctest::Approx(7.5f));
}

TEST_CASE("armor penetration eats positive mitigation only, floor 0") {
    // Baseline (TypedDamageTest): slash 100 → flat 10 → 90 → armor 10% → 81.
    Fixture f;
    DamageEvent event;
    event.channels.push_back({ DamageType::Slash, 100.0f });
    event.armorPenetration = 4.0f; // armor 10% → 6% → 90 × 0.94 = 84.6
    StatBlock b = f.block();
    CHECK(applyDamage(b, event, f.tags, f.derived).healthDamage ==
          doctest::Approx(84.6f));

    Fixture g;
    DamageEvent overkill;
    overkill.channels.push_back({ DamageType::Slash, 100.0f });
    overkill.armorPenetration = 1000.0f; // floors at 0%, never negative
    StatBlock gb = g.block();
    CHECK(applyDamage(gb, overkill, g.tags, g.derived).healthDamage ==
          doctest::Approx(90.0f));
}

TEST_CASE("penetration never amplifies an existing vulnerability") {
    Fixture f;
    // Force a vulnerability: resistFire current 10 → −20 via modifiers.
    StatModifiers mods;
    mods.add[attr("resistFire")] = -30.0f;
    recomputeStats(f.core, f.vitals, f.system, f.derived, &mods);
    DamageEvent event;
    event.channels.push_back({ DamageType::Fire, 100.0f });
    event.resistPenetration = 50.0f; // must NOT push −20 further down
    StatBlock b = f.block();
    // fire 100 → flat will 6 → 94 → −20% resist = ×1.2 → 112.8.
    CHECK(applyDamage(b, event, f.tags, f.derived, &mods).healthDamage ==
          doctest::Approx(112.8f));
}

TEST_CASE("critical execution: sensitivity% of maxHealth × multiplier, no armor") {
    Fixture f;
    const f32 maxHealth = currentValueOf(f.system, attr("maxHealth"));
    const f32 sensitivity =
        currentValueOf(f.system, attr("criticalSensitivity"));
    DamageEvent event;
    event.critical = true;
    event.criticalMultiplier = 2.0f;
    StatBlock b = f.block();
    const DamageResult r = applyDamage(b, event, f.tags, f.derived);
    CHECK(r.healthDamage ==
          doctest::Approx(maxHealth * sensitivity / 100.0f * 2.0f));
    CHECK(r.healthDamage > 0.0f); // the default sheet has a real window
}

TEST_CASE("bleed burst: criticalSensitivity% of maxHealth, ignores armor") {
    // The status bleed burst (CharacterTick / GameTime) applies a channel-free
    // critical execution with multiplier 1: criticalSensitivity% of maxHealth,
    // bypassing armor. Regression for the old flat armor-mitigated slash.
    Fixture f;
    // Heavy slash armor must NOT reduce the bleed chunk (it has no channels to
    // mitigate — that is exactly what "ignores armor" means here).
    StatModifiers armor;
    armor.add[attr("armorSlash")] = 80.0f;
    recomputeStats(f.core, f.vitals, f.system, f.derived, &armor);
    const f32 maxHealth = currentValueOf(f.system, attr("maxHealth"));
    const f32 sensitivity =
        currentValueOf(f.system, attr("criticalSensitivity"));

    DamageEvent bleed; // exactly what the tick sites build
    bleed.critical = true;
    bleed.criticalMultiplier = 1.0f;
    StatBlock b = f.block();
    const DamageResult r = applyDamage(b, bleed, f.tags, f.derived, &armor);
    CHECK(r.healthDamage == doctest::Approx(maxHealth * sensitivity / 100.0f));
    CHECK(r.healthDamage > 0.0f);
}
