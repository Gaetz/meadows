#include <doctest/doctest.h>

#include "engine/core/Rng.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/Attributes.hpp"
#include "gameplay/ability/GameplayEffects.hpp"
#include "gameplay/ability/GameplayTags.hpp"
#include "gameplay/stats/Afflictions.hpp"

using namespace gameplay;

namespace {
// Build an affliction EffectForm targeting the given channel attribute.
EffectForm makeAffliction(const char* grantedTag, const char* channelAttr,
                          f32 resonancePenalty, const char* attrMalus, f32 malusVal,
                          f32 recoveryHours) {
    EffectForm e;
    e.attribute   = channelAttr;
    e.op          = "add";
    e.magnitude   = resonancePenalty;
    e.attribute2  = attrMalus ? attrMalus : "";
    e.magnitude2  = malusVal;
    e.durationHours = recoveryHours;
    e.grantedTag  = grantedTag;
    return e;
}
} // namespace

TEST_CASE("afflictions: immune when channel resonance >= 0") {
    GameplayTagRegistry tags;
    tags.registerTag("Status.Diseased.Fever");
    AttributeSet vitals;
    AbilitySystem sys;
    core::Rng rng(1);

    const EffectForm fever =
        makeAffliction("Status.Diseased.Fever", "amber", -15.0f, "constitution", -2.0f, 48.0f);
    CHECK_FALSE(inflictEffect(vitals, sys, fever, 0.0f, 1.0, rng, tags)); // amber >= 0 → immune
    CHECK(sys.activeEffects.empty());
}

TEST_CASE("afflictions: disease adds amber resonance GAS effect and an attribute malus") {
    GameplayTagRegistry tags;
    tags.registerTag("Status.Diseased.Fever");
    AttributeSet vitals;
    AbilitySystem sys;
    core::Rng rng(1);

    const EffectForm fever =
        makeAffliction("Status.Diseased.Fever", "amber", -15.0f, "constitution", -2.0f, 48.0f);
    CHECK(inflictEffect(vitals, sys, fever, -100.0f, 1.0, rng, tags));

    f32 amberPenalty = 0.0f, constitutionMalus = 0.0f;
    for (const auto& ae : sys.activeEffects) {
        if (ae.attribute == attr("amber"))       amberPenalty     += ae.magnitude;
        if (ae.attribute == attr("constitution")) constitutionMalus += ae.magnitude;
        CHECK(ae.gameTime);
        CHECK(ae.remaining == doctest::Approx(48.0f * 3600.0f));
    }
    CHECK(amberPenalty     == doctest::Approx(-15.0f));
    CHECK(constitutionMalus == doctest::Approx(-2.0f));
}

TEST_CASE("afflictions: a psychosis hits the garnet (essence) channel") {
    GameplayTagRegistry tags;
    tags.registerTag("Status.Mental.Phobia");
    AttributeSet vitals;
    AbilitySystem sys;
    core::Rng rng(1);

    const EffectForm phobia =
        makeAffliction("Status.Mental.Phobia", "garnet", -20.0f, "", 0.0f, 72.0f);
    CHECK(inflictEffect(vitals, sys, phobia, -100.0f, 1.0, rng, tags));

    f32 garnetPenalty = 0.0f;
    for (const auto& ae : sys.activeEffects) {
        if (ae.attribute == attr("garnet")) garnetPenalty += ae.magnitude;
    }
    CHECK(garnetPenalty == doctest::Approx(-20.0f));
}

TEST_CASE("afflictions: re-infliction refreshes the timer, not stack") {
    GameplayTagRegistry tags;
    tags.registerTag("Status.Diseased.Fever");
    AttributeSet vitals;
    AbilitySystem sys;
    core::Rng rng(1);

    const EffectForm fever =
        makeAffliction("Status.Diseased.Fever", "amber", -15.0f, "", 0.0f, 48.0f);
    CHECK(inflictEffect(vitals, sys, fever, -100.0f, 1.0, rng, tags));
    const auto countAfterFirst = sys.activeEffects.size();

    // Partial tick, then re-inflict — effect count stays same, timer resets.
    tickGameTimeEffects(vitals, sys, 1.0 * 3600.0, tags);
    CHECK(inflictEffect(vitals, sys, fever, -100.0f, 1.0, rng, tags));
    CHECK(sys.activeEffects.size() == countAfterFirst);
    for (const auto& ae : sys.activeEffects) {
        CHECK(ae.remaining == doctest::Approx(48.0f * 3600.0f));
    }
}

TEST_CASE("afflictions: effect expires after durationHours via tickGameTimeEffects") {
    GameplayTagRegistry tags;
    tags.registerTag("Status.Diseased.Fever");
    AttributeSet vitals;
    AbilitySystem sys;
    core::Rng rng(1);

    const EffectForm fever =
        makeAffliction("Status.Diseased.Fever", "amber", -15.0f, "", 0.0f, 48.0f);
    CHECK(inflictEffect(vitals, sys, fever, -100.0f, 1.0, rng, tags));
    CHECK_FALSE(sys.activeEffects.empty());

    tickGameTimeEffects(vitals, sys, 48.0 * 3600.0, tags);
    CHECK(sys.activeEffects.empty());
}
