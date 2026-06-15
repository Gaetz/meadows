#include <doctest/doctest.h>

#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/GameplayEffects.hpp"
#include "gameplay/stats/CharacterStats.hpp"
#include "gameplay/stats/CoreAttributes.hpp"
#include "gameplay/stats/StatsTuning.hpp"
#include "gameplay/stats/Survival.hpp"

using namespace gameplay;

namespace {
f32 maxHealthWith(const StatsTuningForm& tuning) {
    DerivedStatRegistry reg;
    registerCoreDerivedStats(reg, tuning);
    CoreAttributes core;
    AttributeSet vitals;
    AbilitySystem sys;
    const AttrSetRef sets[] = {
        { &CoreAttributes::staticTypeInfo(), &core },
        { &AttributeSet::staticTypeInfo(), &vitals },
    };
    recomputeCurrent(sys, sets, &reg, nullptr);
    return currentValueOf(sys, attr("maxHealth"));
}
} // namespace

TEST_CASE("stats tuning: defaults reproduce the Phase-6 constants") {
    CHECK(maxHealthWith(StatsTuningForm {}) == doctest::Approx(90.0f)); // (6+6+6)×5
}

TEST_CASE("stats tuning: a patched constant changes the derived value") {
    StatsTuningForm t;
    t.attributeToMax = 10.0f;
    CHECK(maxHealthWith(t) == doctest::Approx(180.0f)); // (6+6+6)×10
}

TEST_CASE("stats tuning: survival thresholds come from tuning") {
    Survival s;
    s.hunger = 50.0f;
    // Default threshold 75 → 50 is below → negative resonance.
    CHECK(hungerResonance(s) == doctest::Approx((75.0f - 50.0f) / 75.0f * -50.0f));
    // Lower the threshold below 50 → no resonance.
    StatsTuningForm t;
    t.survivalThreshold = 40.0f;
    CHECK(hungerResonance(s, t) == doctest::Approx(0.0f));
}
