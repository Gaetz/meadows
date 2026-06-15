#include <doctest/doctest.h>

#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/GameplayEffects.hpp"
#include "gameplay/stats/CharacterStats.hpp"
#include "gameplay/stats/CoreAttributes.hpp"
#include "gameplay/stats/Resonance.hpp"

using namespace gameplay;

TEST_CASE("harmony: a displacement cascades half then quarter to the next channels") {
    Resonance res;
    res.onyx = -10.0f;
    const Resonance eff = harmonyEffective(res);
    CHECK(eff.onyx == doctest::Approx(-10.0f));
    CHECK(eff.amber == doctest::Approx(-5.0f));  // half
    CHECK(eff.garnet == doctest::Approx(-2.0f)); // quarter, truncated (-2.5 → -2)
}

TEST_CASE("harmony: cascade is a one-shot minimum from the most-displaced channel") {
    Resonance res;
    res.onyx = -10.0f;
    res.amber = -3.0f;
    const Resonance eff = harmonyEffective(res);
    CHECK(eff.amber == doctest::Approx(-5.0f));  // cascade floor wins over own -3
    CHECK(eff.garnet == doctest::Approx(-2.0f)); // from onyx, no second cascade

    Resonance res2;
    res2.onyx = -10.0f;
    res2.amber = -8.0f;
    const Resonance eff2 = harmonyEffective(res2);
    CHECK(eff2.amber == doctest::Approx(-8.0f)); // own displacement larger, kept
}

TEST_CASE("resonance: scales the max and offsets linked attributes via recompute") {
    DerivedStatRegistry reg;
    registerCoreDerivedStats(reg);
    CoreAttributes core;
    AttributeSet vitals;
    AbilitySystem sys;

    Resonance res;
    res.onyx = -30.0f;
    StatModifiers mods;
    buildResonanceModifiers(res, mods);

    const AttrSetRef sets[] = {
        { &CoreAttributes::staticTypeInfo(), &core },
        { &AttributeSet::staticTypeInfo(), &vitals },
    };
    recomputeCurrent(sys, sets, &reg, &mods);

    // Max reads BASE attributes (unaffected by the offset) and only takes the %:
    // onyx -30 → maxHealth = 90 × 0.70 = 63. The offset still lowers the CURRENT
    // attribute (for secondary stats): strength 6 → 4.
    CHECK(currentValueOf(sys, attr("strength")) == doctest::Approx(4.0f));
    CHECK(currentValueOf(sys, attr("maxHealth")) == doctest::Approx(63.0f));
    // harmony cascade: amber -15 → dexterity 6 → 5 (current); maxEnergy = 90 × 0.85.
    CHECK(currentValueOf(sys, attr("dexterity")) == doctest::Approx(5.0f));
    CHECK(currentValueOf(sys, attr("maxEnergy")) == doctest::Approx(76.5f));
}
