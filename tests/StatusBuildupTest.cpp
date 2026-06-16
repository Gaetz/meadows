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
    const auto triggered = tickBuildup(b, sys, 2.0f, tags); // decay 5 × 2 = 10
    CHECK(triggered.empty());
    CHECK(b.poison == doctest::Approx(40.0f));
}

TEST_CASE("status buildup: reaching endurance triggers the status tag and resets") {
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
    const auto triggered = tickBuildup(b, sys, 0.0f, tags);
    REQUIRE(triggered.size() == 1);
    CHECK(triggered[0] == StatusType::Poison);
    CHECK(b.poison == doctest::Approx(0.0f));
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
    CHECK(tickBuildup(b, sys, 0.0f, tags).empty());
}

TEST_CASE("status buildup: status damage scales with the attribute") {
    CHECK(scaledStatusDamage(100.0f, 20.0f) == doctest::Approx(110.0f));
    CHECK(scaledStatusDamage(100.0f, 10.0f) == doctest::Approx(100.0f));
}
