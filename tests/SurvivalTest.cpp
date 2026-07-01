#include <doctest/doctest.h>

#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/Attributes.hpp"
#include "gameplay/ability/GameplayEffects.hpp"
#include "gameplay/ability/GameplayTags.hpp"
#include "gameplay/stats/GameClock.hpp"
#include "gameplay/stats/StatsTuning.hpp"
#include "gameplay/stats/Survival.hpp"

using namespace gameplay;

namespace {
GameplayTagRegistry makeSurvivalTags() {
    GameplayTagRegistry tags;
    registerSurvivalTags(tags);
    return tags;
}
f32 effectOnAttr(const AbilitySystem& sys, const char* attribute) {
    f32 sum = 0.0f;
    const u32 id = attr(attribute);
    for (const auto& ae : sys.activeEffects) {
        if (ae.attribute == id) sum += ae.magnitude;
    }
    return sum;
}
} // namespace

TEST_CASE("survival: hunger starts at 100 and reaches threshold 75 after 24 game hours") {
    StatsTuningForm tuning;
    Survival s;
    CHECK(s.hunger == doctest::Approx(100.0f));
    tickSurvival(s, 12.0 * 3600.0, tuning); // 12h -> -12.5 pts -> 87.5
    CHECK(s.hunger == doctest::Approx(87.5f).epsilon(0.01f));
    tickSurvival(s, 12.0 * 3600.0, tuning); // 12h more -> 75 (threshold)
    CHECK(s.hunger == doctest::Approx(75.0f).epsilon(0.01f));
}

TEST_CASE("survival: hunger below 75 creates a negative amber GAS effect, linearly") {
    StatsTuningForm tuning;
    GameplayTagRegistry tags = makeSurvivalTags();
    AttributeSet vitals;
    AbilitySystem sys;
    Survival s;

    s.hunger = 75.0f;
    updateSurvivalEffects(s, sys, vitals, tags, tuning);
    CHECK(effectOnAttr(sys, "amber") == doctest::Approx(0.0f)); // at threshold: no effect

    s.hunger = 90.0f;
    updateSurvivalEffects(s, sys, vitals, tags, tuning);
    CHECK(effectOnAttr(sys, "amber") == doctest::Approx(0.0f)); // above threshold: none

    s.hunger = 37.5f;
    updateSurvivalEffects(s, sys, vitals, tags, tuning);
    CHECK(effectOnAttr(sys, "amber") == doctest::Approx(-25.0f)); // halfway -> -25

    s.hunger = 0.0f;
    updateSurvivalEffects(s, sys, vitals, tags, tuning);
    CHECK(effectOnAttr(sys, "amber") == doctest::Approx(-50.0f)); // empty -> -50
}

TEST_CASE("survival: updateSurvivalEffects replaces previous effect (no stacking)") {
    StatsTuningForm tuning;
    GameplayTagRegistry tags = makeSurvivalTags();
    AttributeSet vitals;
    AbilitySystem sys;
    Survival s;

    s.hunger = 37.5f;
    updateSurvivalEffects(s, sys, vitals, tags, tuning);
    CHECK(effectOnAttr(sys, "amber") == doctest::Approx(-25.0f));

    // Update to a different level — only one effect should remain.
    s.hunger = 0.0f;
    updateSurvivalEffects(s, sys, vitals, tags, tuning);
    CHECK(effectOnAttr(sys, "amber") == doctest::Approx(-50.0f)); // replaced, not stacked
}

TEST_CASE("survival: low sleep drives garnet resonance effect, hunger/thirst drive amber") {
    StatsTuningForm tuning;
    GameplayTagRegistry tags = makeSurvivalTags();
    AttributeSet vitals;
    AbilitySystem sys;
    Survival s;

    s.sleep = 0.0f; // hunger/thirst full -> only garnet affected
    updateSurvivalEffects(s, sys, vitals, tags, tuning);
    CHECK(effectOnAttr(sys, "garnet") == doctest::Approx(-50.0f));
    CHECK(effectOnAttr(sys, "amber")  == doctest::Approx(0.0f));

    s.hunger = 0.0f;
    s.thirst = 0.0f;
    updateSurvivalEffects(s, sys, vitals, tags, tuning);
    CHECK(effectOnAttr(sys, "amber") == doctest::Approx(-100.0f)); // hunger -50 + thirst -50
}

TEST_CASE("survival: advancing the clock drives hunger below threshold -> effect") {
    StatsTuningForm tuning;
    GameplayTagRegistry tags = makeSurvivalTags();
    AttributeSet vitals;
    AbilitySystem sys;
    GameClock clock;
    Survival s;

    const f64 gameDt = clock.advance(30000.0f); // ~83 game hours
    tickSurvival(s, gameDt, tuning);
    CHECK(s.hunger < 75.0f);
    updateSurvivalEffects(s, sys, vitals, tags, tuning);
    CHECK(effectOnAttr(sys, "amber") < 0.0f);
}
