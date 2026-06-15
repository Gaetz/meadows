#include <doctest/doctest.h>

#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/GameplayEffects.hpp"
#include "gameplay/stats/CharacterStats.hpp"
#include "gameplay/stats/CoreAttributes.hpp"

using namespace gameplay;

namespace {
// Recompute over a CoreAttributes + Vitals pair with the derived registry.
void recompute(AbilitySystem& sys, const CoreAttributes& core,
               const AttributeSet& vitals, const DerivedStatRegistry& reg) {
    const AttrSetRef sets[] = {
        { &CoreAttributes::staticTypeInfo(), &core },
        { &AttributeSet::staticTypeInfo(), &vitals },
    };
    recomputeCurrent(sys, sets, &reg);
}
} // namespace

TEST_CASE("derived stats: primary maxima derive from the nine attributes") {
    DerivedStatRegistry reg;
    registerCoreDerivedStats(reg);

    CoreAttributes core;  // 6/6/6 per group, insight 0
    AttributeSet vitals;  // authored maxHealth 100 — overwritten by the formula
    AbilitySystem sys;

    recompute(sys, core, vitals, reg);
    CHECK(currentValueOf(sys, attr("maxHealth")) == doctest::Approx(90.0f));
    CHECK(currentValueOf(sys, attr("maxEnergy")) == doctest::Approx(90.0f));
    CHECK(currentValueOf(sys, attr("maxEssence")) == doctest::Approx(60.0f));
}

TEST_CASE("derived stats: bumping an attribute recomputes its maximum") {
    DerivedStatRegistry reg;
    registerCoreDerivedStats(reg);
    CoreAttributes core;
    AttributeSet vitals;
    AbilitySystem sys;

    core.strength = 10.0f;
    recompute(sys, core, vitals, reg);
    CHECK(currentValueOf(sys, attr("maxHealth")) ==
          doctest::Approx((10.0f + 6.0f + 6.0f) * 5.0f)); // 110
}

TEST_CASE("derived stats: an infinite Override effect pins a stat (non-humanoid)") {
    DerivedStatRegistry reg;
    registerCoreDerivedStats(reg);
    CoreAttributes core;
    AttributeSet vitals;
    AbilitySystem sys;

    // A monster bypasses the humanoid formula with an infinite Override effect.
    ActiveEffect over;
    over.attribute = attr("maxHealth");
    over.op = ModifierOp::Override;
    over.magnitude = 500.0f;
    over.infinite = true;
    sys.activeEffects.push_back(over);

    recompute(sys, core, vitals, reg);
    CHECK(currentValueOf(sys, attr("maxHealth")) == doctest::Approx(500.0f));
}

TEST_CASE("derived stats: an actor without CoreAttributes keeps its authored max") {
    DerivedStatRegistry reg;
    registerCoreDerivedStats(reg);
    AttributeSet vitals;      // like the TrainingDummy: authored max, no attributes
    vitals.maxHealth = 40.0f;
    AbilitySystem sys;

    // Only Vitals present → the maxHealth calculator's source (CoreAttributes) is
    // absent → opt-in skip → the authored base is preserved (no regression).
    const AttrSetRef sets[] = { { &AttributeSet::staticTypeInfo(), &vitals } };
    recomputeCurrent(sys, sets, &reg);
    CHECK(currentValueOf(sys, attr("maxHealth")) == doctest::Approx(40.0f));
}
