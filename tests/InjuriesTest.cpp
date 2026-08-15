#include <doctest/doctest.h>

#include "engine/core/Rng.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/Attributes.hpp"
#include "gameplay/ability/GameplayEffects.hpp"
#include "gameplay/ability/GameplayTags.hpp"
#include "gameplay/stats/CharacterStats.hpp"
#include "gameplay/stats/CoreAttributes.hpp"
#include "gameplay/stats/Injuries.hpp"

using namespace gameplay;

TEST_CASE("injuries: a cut applies its resonance penalty and attribute malus via GAS") {
    GameplayTagRegistry tags;
    registerInjuryTags(tags);
    AttributeSet vitals;
    AbilitySystem sys;
    Injuries injuries;

    addInjury(injuries, InjuryType::Cut, BodyPart::Torso); // light
    addInjury(injuries, InjuryType::Cut, BodyPart::Torso); // -> major (sev 1)
    REQUIRE(injuries.list.size() == 1);
    CHECK(injuries.list[0].severity == 1);

    syncInjuryEffects(injuries, sys, vitals, tags);

    f32 onyxPenalty = 0.0f, constitutionMalus = 0.0f;
    for (const auto& ae : sys.activeEffects) {
        if (ae.attribute == attr("onyx"))        onyxPenalty      += ae.magnitude;
        if (ae.attribute == attr("constitution")) constitutionMalus += ae.magnitude;
    }
    CHECK(onyxPenalty      == doctest::Approx(-2.0f));  // major cut resonance penalty
    CHECK(constitutionMalus == doctest::Approx(-2.0f)); // torso cut attribute malus
}

TEST_CASE("injuries: a leg fracture multiplies movement speed down via GAS") {
    GameplayTagRegistry tags;
    registerInjuryTags(tags);
    AttributeSet vitals;
    AbilitySystem sys;
    Injuries injuries;

    addInjury(injuries, InjuryType::Fracture, BodyPart::Legs); // light -> speed -10%
    syncInjuryEffects(injuries, sys, vitals, tags);

    f32 speedMul = 1.0f;
    for (const auto& ae : sys.activeEffects) {
        if (ae.attribute == attr("movementSpeed") && ae.op == ModifierOp::Multiply) {
            speedMul *= ae.magnitude;
        }
    }
    CHECK(speedMul == doctest::Approx(0.9f));
}

TEST_CASE("injuries: maluses flow through the recompute") {
    GameplayTagRegistry tags;
    registerInjuryTags(tags);
    CoreAttributes core; // constitution 6
    AttributeSet vitals;
    AbilitySystem sys;
    Injuries injuries;

    addInjury(injuries, InjuryType::Cut, BodyPart::Torso); // constitution -1
    syncInjuryEffects(injuries, sys, vitals, tags);

    const AttrSetRef sets[] = {
        { &CoreAttributes::staticTypeInfo(), &core },
        { &AttributeSet::staticTypeInfo(),   &vitals },
    };
    recomputeCurrent(sys, sets, nullptr);
    CHECK(currentValueOf(sys, attr("constitution")) == doctest::Approx(5.0f)); // 6-1
}

TEST_CASE("injuries: inflict is gated by resonance-resistance (onyx >= 0 -> immune)") {
    core::Rng rng(1);
    Injuries injuries;
    CHECK_FALSE(rollInjury(injuries, InjuryType::Cut, BodyPart::Arms, 1.0, 0.0f, rng));
    CHECK(injuries.list.empty());
    CHECK(rollInjury(injuries, InjuryType::Cut, BodyPart::Arms, 1.0, -100.0f, rng));
    CHECK(injuries.list.size() == 1);
}

TEST_CASE("injuries: base chance follows the per-type health thresholds") {
    CHECK(injuryBaseChance(InjuryType::Cut,      0.20f) == doctest::Approx(0.20));
    CHECK(injuryBaseChance(InjuryType::Cut,      0.05f) == doctest::Approx(0.0)); // <10%
    CHECK(injuryBaseChance(InjuryType::Fracture, 0.60f) == doctest::Approx(0.5));
    CHECK(injuryBaseChance(InjuryType::Fracture, 0.40f) == doctest::Approx(0.0));
}

TEST_CASE("injuries: rest recovers a rank, then clears the injury") {
    Injuries injuries;
    addInjury(injuries, InjuryType::Bruise, BodyPart::Head); // light, recovery 24h
    recoverInjuries(injuries, 12.0f);
    REQUIRE(injuries.list.size() == 1);
    CHECK(injuries.list[0].severity == 0);
    recoverInjuries(injuries, 12.0f); // 24h total -> light clears
    CHECK(injuries.list.empty());
}

TEST_CASE("injuries: syncInjuryEffects removes old effects and re-applies fresh ones") {
    GameplayTagRegistry tags;
    registerInjuryTags(tags);
    AttributeSet vitals;
    AbilitySystem sys;
    Injuries injuries;

    addInjury(injuries, InjuryType::Cut, BodyPart::Torso);
    syncInjuryEffects(injuries, sys, vitals, tags);
    const auto countBefore = sys.activeEffects.size();
    CHECK(countBefore > 0);

    // Clear all injuries and sync -> no more injury effects.
    injuries.list.clear();
    syncInjuryEffects(injuries, sys, vitals, tags);
    CHECK(sys.activeEffects.empty());
}
