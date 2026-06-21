#include <doctest/doctest.h>

#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/GameplayTags.hpp"
#include "gameplay/stats/Drugs.hpp"
#include "gameplay/stats/Resonance.hpp"

using namespace gameplay;

namespace {
DrugForm stimulant() {
    DrugForm drug;
    drug.channel = "amber";
    drug.resonanceBoost = 100.0f;
    drug.durationHours = 2.0f;
    drug.aftershockResonance = -30.0f;
    drug.aftershockRecoveryPerHour = 1.0f;
    return drug;
}
} // namespace

TEST_CASE("drugs: taking one boosts its channel and breaks harmony") {
    GameplayTagRegistry tags;
    tags.registerTag("Status.HarmonyBroken");
    AbilitySystem sys;
    ActiveDrugs drugs;

    takeDrug(drugs, stimulant(), sys, tags);
    REQUIRE(drugs.list.size() == 1);
    CHECK(harmonyBroken(sys, tags));
    CHECK(drugResonance(drugs).amber == doctest::Approx(100.0f));
}

TEST_CASE("drugs: harmony break makes the channels independent (no cascade)") {
    Resonance res;
    res.onyx = -10.0f; // with harmony: amber −5, garnet −2; broken: onyx only.
    StatModifiers withHarmony;
    buildResonanceModifiers(res, withHarmony, false);
    StatModifiers broken;
    buildResonanceModifiers(res, broken, true);

    CHECK(withHarmony.mul[attr("maxEnergy")] == doctest::Approx(0.95f)); // amber −5
    CHECK(broken.mul[attr("maxEnergy")] == doctest::Approx(1.0f));       // amber 0
    // The source channel is the same either way.
    CHECK(withHarmony.mul[attr("maxHealth")] == doctest::Approx(0.90f));
    CHECK(broken.mul[attr("maxHealth")] == doctest::Approx(0.90f));
}

TEST_CASE("drugs: aftershock becomes a progressive aftereffect, not an instant persistent hit") {
    GameplayTagRegistry tags;
    tags.registerTag("Status.HarmonyBroken");
    AbilitySystem sys;
    ActiveDrugs drugs;
    takeDrug(drugs, stimulant(), sys, tags);

    tickDrugs(drugs, sys, 1.0 * 3600.0, tags); // 1h — still active
    CHECK(drugs.list.size() == 1);
    CHECK(harmonyBroken(sys, tags));
    CHECK(drugAftereffectResonance(drugs).amber == doctest::Approx(0.0f)); // not expired yet

    tickDrugs(drugs, sys, 2.0 * 3600.0, tags); // +2h — wears off
    CHECK(drugs.list.empty());
    CHECK_FALSE(harmonyBroken(sys, tags));
    // Aftereffect created: -30 amber, not touched persistent resonance.
    REQUIRE(drugs.aftereffects.size() == 1);
    CHECK(drugAftereffectResonance(drugs).amber == doctest::Approx(-30.0f));
}

TEST_CASE("drugs: aftereffect recovers at 1pt/game-hour toward 0") {
    GameplayTagRegistry tags;
    tags.registerTag("Status.HarmonyBroken");
    AbilitySystem sys;
    ActiveDrugs drugs;
    takeDrug(drugs, stimulant(), sys, tags);
    tickDrugs(drugs, sys, 2.0 * 3600.0, tags); // wear off → aftereffect -30
    REQUIRE(drugs.aftereffects.size() == 1);

    tickDrugs(drugs, sys, 10.0 * 3600.0, tags); // 10h recovery → -30 + 10 = -20
    CHECK(drugAftereffectResonance(drugs).amber == doctest::Approx(-20.0f));

    tickDrugs(drugs, sys, 20.0 * 3600.0, tags); // 20h more → fully recovered
    CHECK(drugs.aftereffects.empty());
    CHECK(drugAftereffectResonance(drugs).amber == doctest::Approx(0.0f));
}

TEST_CASE("drugs: multiple aftereffects stack independently") {
    GameplayTagRegistry tags;
    tags.registerTag("Status.HarmonyBroken");
    AbilitySystem sys;
    ActiveDrugs drugs;

    // Take two drugs in succession, both expire.
    takeDrug(drugs, stimulant(), sys, tags);
    takeDrug(drugs, stimulant(), sys, tags);
    tickDrugs(drugs, sys, 3.0 * 3600.0, tags); // both expire → two aftereffects

    CHECK(drugs.list.empty());
    CHECK(drugs.aftereffects.size() == 2);
    CHECK(drugAftereffectResonance(drugs).amber == doctest::Approx(-60.0f)); // -30 × 2

    tickDrugs(drugs, sys, 30.0 * 3600.0, tags); // 30h → both fully recovered
    CHECK(drugs.aftereffects.empty());
}
