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

TEST_CASE("derived stats: the AUTHORED balance override pins maxHealth "
          "(STATS.md `override ?? formula` — the archer-flees bug)") {
    DerivedStatRegistry reg;
    registerCoreDerivedStats(reg);
    CoreAttributes core;
    AttributeSet vitals; // struct default maxHealth 100 does NOT pin...
    AbilitySystem sys;

    recompute(sys, core, vitals, reg);
    CHECK(currentValueOf(sys, attr("maxHealth")) == doctest::Approx(90.0f));

    // ...the dedicated override field does (the Spawner seeds it from
    // ActorForm.maxHealth): the bandit really has its authored 35.
    vitals.maxHealthOverride = 35.0f;
    recompute(sys, core, vitals, reg);
    CHECK(currentValueOf(sys, attr("maxHealth")) == doctest::Approx(35.0f));

    // 0 = back to the attribute formula (the Player's record).
    vitals.maxHealthOverride = 0.0f;
    recompute(sys, core, vitals, reg);
    CHECK(currentValueOf(sys, attr("maxHealth")) == doctest::Approx(90.0f));
}

TEST_CASE("effects: an instant effect no longer snaps health to a stale "
          "base max (the archer fled at 39% after ONE attack cost)") {
    DerivedStatRegistry reg;
    registerCoreDerivedStats(reg);
    GameplayTagRegistry tags;
    CoreAttributes core; // defaults: derived maxHealth would be 90
    AttributeSet vitals;
    AbilitySystem sys;

    // The Spawner's archer seed: authored 35 into max/health/override...
    vitals.maxHealth = 35.0f;
    vitals.health = 35.0f;
    vitals.maxHealthOverride = 35.0f;
    initializeCurrent(sys, vitals);
    recompute(sys, core, vitals, reg);
    // ...the override makes the max REALLY 35: full health, no 35/90.
    CHECK(currentValueOf(sys, attr("maxHealth")) == doctest::Approx(35.0f));
    vitals.health = currentValueOf(sys, attr("maxHealth"));
    recompute(sys, core, vitals, reg);

    // The first instant effect (the attack ability's energy cost) used
    // to clampBaseVitals health against the stale BASE max — the fresh
    // 90 snapped to 35 (fraction 39% -> the archer fled). Now the clamp
    // reads the CURRENT max: full health stays full.
    EffectForm cost;
    cost.attribute = "energy";
    cost.op = "add";
    cost.magnitude = -5.0f;
    cost.duration = "instant";
    CHECK(applyEffect(vitals, sys, cost, tags));
    const f32 fraction = currentValueOf(sys, attr("health")) /
                         currentValueOf(sys, attr("maxHealth"));
    CHECK(fraction == doctest::Approx(1.0f));
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

TEST_CASE("derived stats: a partial recompute preserves the derived maxima "
          "(audit U6-F10)") {
    DerivedStatRegistry reg;
    registerCoreDerivedStats(reg);
    CoreAttributes core;
    AttributeSet vitals;
    AbilitySystem sys;

    core.strength = 10.0f;
    recompute(sys, core, vitals, reg); // full: maxHealth = formula (110)
    REQUIRE(currentValueOf(sys, attr("maxHealth")) == doctest::Approx(110.0f));

    // An instant effect triggers the 2-arg (partial, no registry) recompute
    // inside applyEffect. It used to clobber maxHealth back to the raw
    // AttributeSet default (100) until the next full recompute; it must now
    // preserve the formula result.
    GameplayTagRegistry tags;
    EffectForm damage;
    damage.attribute = "health";
    damage.op = "add";
    damage.magnitude = -10.0f;
    damage.duration = "instant";
    applyEffect(vitals, sys, damage, tags);
    CHECK(currentValueOf(sys, attr("maxHealth")) == doctest::Approx(110.0f));
    CHECK(currentValueOf(sys, attr("health")) == doctest::Approx(90.0f));

    // The plain (non-derived) fields still recompute from the base.
    CHECK(currentValueOf(sys, attr("armorRating")) == doctest::Approx(0.0f));
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
