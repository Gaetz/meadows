#include <doctest/doctest.h>

#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/Attributes.hpp"
#include "gameplay/ability/GameplayEffects.hpp"
#include "gameplay/ability/GameplayTags.hpp"
#include "gameplay/stats/Drugs.hpp"
#include "gameplay/stats/Resonance.hpp"

using namespace gameplay;

namespace {
EffectForm stimulantEffect() {
    EffectForm e;
    e.attribute      = "amber";
    e.op             = "add";
    e.magnitude      = 100.0f;
    e.durationHours  = 2.0f;
    e.grantedTag     = "Status.HarmonyBroken";
    e.expiryMode     = "decay";
    e.expiryMagnitude = -30.0f;
    e.decayPerHour   = 1.0f;
    return e;
}
} // namespace

TEST_CASE("drugs: taking one boosts amber channel and breaks harmony") {
    GameplayTagRegistry tags;
    tags.registerTag("Status.HarmonyBroken");
    AttributeSet vitals;
    AbilitySystem sys;

    applyEffect(vitals, sys, stimulantEffect(), tags);
    CHECK(harmonyBroken(sys, tags));

    bool hasAmberBoost = false;
    for (const auto& ae : sys.activeEffects) {
        if (ae.attribute == attr("amber") && ae.magnitude == doctest::Approx(100.0f)) {
            hasAmberBoost = true;
            CHECK(ae.gameTime);
        }
    }
    CHECK(hasAmberBoost);
}

TEST_CASE("drugs: harmony break makes the channels independent (no cascade)") {
    Resonance res;
    res.onyx = -10.0f; // with harmony: amber -5, garnet -2; broken: onyx only.
    StatModifiers withHarmony;
    buildResonanceModifiers(res, withHarmony, false);
    StatModifiers broken;
    buildResonanceModifiers(res, broken, true);

    CHECK(withHarmony.mul[attr("maxEnergy")] == doctest::Approx(0.95f)); // amber -5
    CHECK(broken.mul[attr("maxEnergy")]      == doctest::Approx(1.0f));  // amber 0
    // The source channel is unaffected either way.
    CHECK(withHarmony.mul[attr("maxHealth")] == doctest::Approx(0.90f));
    CHECK(broken.mul[attr("maxHealth")]      == doctest::Approx(0.90f));
}

TEST_CASE("drugs: effect expires after durationHours, harmony no longer broken") {
    GameplayTagRegistry tags;
    tags.registerTag("Status.HarmonyBroken");
    AttributeSet vitals;
    AbilitySystem sys;

    applyEffect(vitals, sys, stimulantEffect(), tags);
    REQUIRE(harmonyBroken(sys, tags));

    tickGameTimeEffects(vitals, sys, 2.0 * 3600.0, tags); // 2h -> wears off
    CHECK_FALSE(harmonyBroken(sys, tags));
    CHECK(sys.activeEffects.empty());
}

TEST_CASE("drugs: taking the same drug twice stacks two active effects") {
    GameplayTagRegistry tags;
    tags.registerTag("Status.HarmonyBroken");
    AttributeSet vitals;
    AbilitySystem sys;

    applyEffect(vitals, sys, stimulantEffect(), tags);
    const auto countFirst = sys.activeEffects.size();

    applyEffect(vitals, sys, stimulantEffect(), tags);
    CHECK(sys.activeEffects.size() == countFirst * 2);
    CHECK(harmonyBroken(sys, tags));
}
