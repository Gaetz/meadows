#include <doctest/doctest.h>

#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/GameplayEffects.hpp"
#include "gameplay/ability/GameplayTags.hpp"
#include "gameplay/stats/CharacterStats.hpp"
#include "gameplay/stats/CoreAttributes.hpp"
#include "gameplay/stats/StatusBuildup.hpp"

using namespace gameplay;

namespace {
void recompute(AbilitySystem& sys, const CoreAttributes& core,
               const DerivedStatRegistry& reg) {
    AttributeSet vitals;
    const AttrSetRef sets[] = {
        { &CoreAttributes::staticTypeInfo(), &core },
        { &AttributeSet::staticTypeInfo(), &vitals },
    };
    recomputeCurrent(sys, sets, &reg, nullptr);
}
} // namespace

TEST_CASE("status buildup: endurance derives the per-type threshold") {
    DerivedStatRegistry reg;
    registerCoreDerivedStats(reg);
    CoreAttributes core;
    core.dexterity = 20.0f; // endurancePoison = 100 + 20 × 0.5
    AbilitySystem sys;
    recompute(sys, core, reg);
    CHECK(currentValueOf(sys, attr("endurancePoison")) == doctest::Approx(110.0f));
}

TEST_CASE("status buildup: accrues, then decays when below the threshold") {
    DerivedStatRegistry reg;
    registerCoreDerivedStats(reg);
    CoreAttributes core;
    core.dexterity = 20.0f; // threshold 110
    AbilitySystem sys;
    recompute(sys, core, reg);
    GameplayTagRegistry tags;
    tags.registerTag("Status.Poisoned");

    StatusBuildup b;
    addBuildup(b, StatusType::Poison, 50.0f);
    CHECK(b.poison == doctest::Approx(50.0f));
    const auto result = tickBuildup(b, sys, 2.0f, tags); // decay 3 × 2 = 6
    CHECK(result.triggered.empty());
    CHECK(b.poison == doctest::Approx(44.0f));
}

TEST_CASE("status buildup: reaching endurance grants tag and clamps to threshold") {
    DerivedStatRegistry reg;
    registerCoreDerivedStats(reg);
    CoreAttributes core;
    core.dexterity = 20.0f; // threshold 110
    AbilitySystem sys;
    recompute(sys, core, reg);
    GameplayTagRegistry tags;
    tags.registerTag("Status.Poisoned");

    StatusBuildup b;
    addBuildup(b, StatusType::Poison, 120.0f); // ≥ 110
    const auto result = tickBuildup(b, sys, 0.0f, tags);
    REQUIRE(result.triggered.size() == 1);
    CHECK(result.triggered[0] == StatusType::Poison);
    CHECK(b.poison == doctest::Approx(110.0f)); // clamped to threshold; decays from here
    const auto tag = tags.find("Status.Poisoned");
    REQUIRE(tag.has_value());
    CHECK(sys.tags.has(*tag));
}

TEST_CASE("status buildup: higher endurance resists the same buildup") {
    DerivedStatRegistry reg;
    registerCoreDerivedStats(reg);
    CoreAttributes core;
    core.dexterity = 60.0f; // endurancePoison = 100 + 60 × 0.5 = 130
    AbilitySystem sys;
    recompute(sys, core, reg);
    GameplayTagRegistry tags;
    tags.registerTag("Status.Poisoned");

    StatusBuildup b;
    addBuildup(b, StatusType::Poison, 120.0f); // < 130
    CHECK(tickBuildup(b, sys, 0.0f, tags).triggered.empty());
}

TEST_CASE("status buildup: status damage scales with the attribute") {
    CHECK(scaledStatusDamage(100.0f, 20.0f) == doctest::Approx(110.0f));
    CHECK(scaledStatusDamage(100.0f, 10.0f) == doctest::Approx(100.0f));
}

TEST_CASE("status buildup: active status decays at 1%/s and produces poison DoT") {
    DerivedStatRegistry reg;
    registerCoreDerivedStats(reg);
    CoreAttributes core; // default alacrity=6 → vitality = min(2.5, 31) = 2.5%
    AbilitySystem sys;
    recompute(sys, core, reg);
    GameplayTagRegistry tags;
    tags.registerTag("Status.Poisoned");

    // Trigger the status first.
    StatusBuildup b;
    addBuildup(b, StatusType::Poison, 200.0f); // ≥ threshold (103)
    tickBuildup(b, sys, 0.0f, tags);           // trigger: b.poison = threshold ≈ 103
    CHECK(sys.tags.has(*tags.find("Status.Poisoned")));

    // Tick 1s: decay 1% of 103 = 1.03 → ~101.97; DoT ≈ 1*(1-0.025) = 0.975 HP.
    const auto result = tickBuildup(b, sys, 1.0f, tags);
    CHECK(result.triggered.empty());
    CHECK(b.poison < 103.0f);
    const doctest::Approx expectedDot = doctest::Approx(0.975f).epsilon(0.01f);
    CHECK(result.poisonHealthDamage == expectedDot);
}

TEST_CASE("status buildup: tryAddBuildup blocked while status active") {
    DerivedStatRegistry reg;
    registerCoreDerivedStats(reg);
    CoreAttributes core;
    AbilitySystem sys;
    recompute(sys, core, reg);
    GameplayTagRegistry tags;
    tags.registerTag("Status.Poisoned");

    StatusBuildup b;
    addBuildup(b, StatusType::Poison, 200.0f);
    tickBuildup(b, sys, 0.0f, tags); // trigger; tag now active
    const f32 valueAfterTrigger = b.poison;

    tryAddBuildup(b, StatusType::Poison, 50.0f, sys, tags); // blocked
    CHECK(b.poison == doctest::Approx(valueAfterTrigger));
}

TEST_CASE("status buildup: status expires when buildup reaches zero") {
    DerivedStatRegistry reg;
    registerCoreDerivedStats(reg);
    CoreAttributes core;
    AbilitySystem sys;
    recompute(sys, core, reg);
    GameplayTagRegistry tags;
    tags.registerTag("Status.Poisoned");

    StatusBuildup b;
    addBuildup(b, StatusType::Poison, 200.0f);
    tickBuildup(b, sys, 0.0f, tags); // trigger; b.poison = threshold ≈ 103
    // Simulate complete decay: the flat decay (1% threshold/s) reaches exactly 0
    // after threshold/decayPerSec ≈ 100s. Set it directly to verify expiry.
    b.poison = 0.0f;
    tickBuildup(b, sys, 0.0f, tags); // value == 0 → tag removed
    CHECK(b.poison == doctest::Approx(0.0f));
    CHECK(!sys.tags.has(*tags.find("Status.Poisoned")));
}

TEST_CASE("status buildup: bleed triggers burst and immediately resets") {
    DerivedStatRegistry reg;
    registerCoreDerivedStats(reg);
    CoreAttributes core;
    AbilitySystem sys;
    recompute(sys, core, reg);
    GameplayTagRegistry tags;
    tags.registerTag("Status.Bleeding");

    StatusBuildup b;
    addBuildup(b, StatusType::Bleed, 200.0f);
    const auto result = tickBuildup(b, sys, 0.0f, tags);
    REQUIRE(result.triggered.size() == 1);
    CHECK(result.triggered[0] == StatusType::Bleed);
    CHECK(result.bleedBurst);
    CHECK(b.bleed == doctest::Approx(0.0f)); // immediate reset
    // Bleed has no persistent tag — the status should not be active.
    const auto bleedTag = tags.find("Status.Bleeding");
    const bool bleedActive = bleedTag && sys.tags.has(*bleedTag);
    CHECK(!bleedActive);
}

TEST_CASE("status buildup: death triggers instant kill and resets") {
    DerivedStatRegistry reg;
    registerCoreDerivedStats(reg);
    CoreAttributes core;
    AbilitySystem sys;
    recompute(sys, core, reg);
    GameplayTagRegistry tags;
    tags.registerTag("Status.Dying");

    StatusBuildup b;
    addBuildup(b, StatusType::Death, 200.0f);
    const auto result = tickBuildup(b, sys, 0.0f, tags);
    REQUIRE(result.triggered.size() == 1);
    CHECK(result.deathTriggered);
    CHECK(b.death == doctest::Approx(0.0f));
}
