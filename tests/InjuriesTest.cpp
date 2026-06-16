#include <doctest/doctest.h>

#include "engine/core/Rng.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/GameplayEffects.hpp"
#include "gameplay/stats/CharacterStats.hpp"
#include "gameplay/stats/CoreAttributes.hpp"
#include "gameplay/stats/Injuries.hpp"

using namespace gameplay;

TEST_CASE("injuries: a cut applies its resonance penalty and attribute malus") {
    Injuries injuries;
    addInjury(injuries, InjuryType::Cut, BodyPart::Torso);          // light
    addInjury(injuries, InjuryType::Cut, BodyPart::Torso);          // → major (sev 1)
    REQUIRE(injuries.list.size() == 1);
    CHECK(injuries.list[0].severity == 1);

    CHECK(injuryResonance(injuries) == doctest::Approx(-2.0f));     // major cut
    StatModifiers mods;
    injuryStatModifiers(injuries, mods);
    CHECK(mods.add[attr("constitution")] == doctest::Approx(-2.0f)); // torso cut
}

TEST_CASE("injuries: a leg fracture multiplies movement speed down") {
    Injuries injuries;
    addInjury(injuries, InjuryType::Fracture, BodyPart::Legs); // light → speed -10%
    StatModifiers mods;
    injuryStatModifiers(injuries, mods);
    CHECK(mods.mul[attr("movementSpeed")] == doctest::Approx(0.9f));
}

TEST_CASE("injuries: maluses flow through the recompute") {
    DerivedStatRegistry reg;
    registerCoreDerivedStats(reg);
    CoreAttributes core; // constitution 6
    AttributeSet vitals;
    AbilitySystem sys;

    Injuries injuries;
    addInjury(injuries, InjuryType::Cut, BodyPart::Torso); // constitution -1
    StatModifiers mods;
    injuryStatModifiers(injuries, mods);

    const AttrSetRef sets[] = {
        { &CoreAttributes::staticTypeInfo(), &core },
        { &AttributeSet::staticTypeInfo(), &vitals },
    };
    recomputeCurrent(sys, sets, &reg, &mods);
    CHECK(currentValueOf(sys, attr("constitution")) == doctest::Approx(5.0f)); // 6-1
}

TEST_CASE("injuries: inflict is gated by resonance-resistance (§2)") {
    core::Rng rng(1);
    Injuries injuries;
    // Non-negative onyx → immune, no matter the base chance.
    CHECK_FALSE(rollInjury(injuries, InjuryType::Cut, BodyPart::Arms, 1.0, 0.0f, rng));
    CHECK(injuries.list.empty());
    // Negative onyx + full base chance × |onyx|/100 = 1.0 → always inflicts.
    CHECK(rollInjury(injuries, InjuryType::Cut, BodyPart::Arms, 1.0, -100.0f, rng));
    CHECK(injuries.list.size() == 1);
}

TEST_CASE("injuries: base chance follows the per-type health thresholds") {
    CHECK(injuryBaseChance(InjuryType::Cut, 0.20f) == doctest::Approx(0.20));
    CHECK(injuryBaseChance(InjuryType::Cut, 0.05f) == doctest::Approx(0.0)); // <10%
    CHECK(injuryBaseChance(InjuryType::Fracture, 0.60f) == doctest::Approx(0.5));
    CHECK(injuryBaseChance(InjuryType::Fracture, 0.40f) == doctest::Approx(0.0));
}

TEST_CASE("injuries: rest recovers a rank, then clears the injury") {
    Injuries injuries;
    addInjury(injuries, InjuryType::Bruise, BodyPart::Head); // light, recovery 24h
    recoverInjuries(injuries, 12.0f); // not enough
    REQUIRE(injuries.list.size() == 1);
    CHECK(injuries.list[0].severity == 0);
    recoverInjuries(injuries, 12.0f); // 24h total → light clears
    CHECK(injuries.list.empty());
}
