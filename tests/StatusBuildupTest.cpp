#include <doctest/doctest.h>

#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/GameplayEffects.hpp"
#include "gameplay/ability/GameplayTags.hpp"
#include "gameplay/stats/CharacterStats.hpp"
#include "gameplay/stats/CoreAttributes.hpp"
#include "gameplay/stats/StatusBuildup.hpp"

using namespace gameplay;

namespace {
void recompute(AbilitySystem& sys, const CoreAttributes& core,
               const DerivedStatRegistry& reg) {
    AttributeSet vitals;
    const AttrSetRef sets[] = {
        { &CoreAttributes::staticTypeInfo(), &core },
        { &AttributeSet::staticTypeInfo(), &vitals },
    };
    recomputeCurrent(sys, sets, &reg, nullptr);
}
} // namespace

TEST_CASE("status buildup: endurance derives the per-type threshold") {
    DerivedStatRegistry reg;
    registerCoreDerivedStats(reg);
    CoreAttributes core;
    core.dexterity = 20.0f; // endurancePoison = 100 + 20 × 0.5
    AbilitySystem sys;
    recompute(sys, core, reg);
    CHECK(currentValueOf(sys, attr("endurancePoison")) == doctest::Approx(110.0f));
}

TEST_CASE("status buildup: accrues, then decays when below the threshold") {
    DerivedStatRegistry reg;
    registerCoreDerivedStats(reg);
    CoreAttributes core;
    core.dexterity = 20.0f; // threshold 110
    AbilitySystem sys;
    recompute(sys, core, reg);
    GameplayTagRegistry tags;
    tags.registerTag("Status.Poisoned");

    StatusBuildup b;
    addBuildup(b, StatusType::Poison, 50.0f);
    CHECK(b.poison == doctest::Approx(50.0f));
    const auto result = tickBuildup(b, sys, 2.0f, tags); // decay 3 × 2 = 6
    CHECK(result.triggered.empty());
    CHECK(b.poison == doctest::Approx(44.0f));
}

TEST_CASE("status buildup: reaching endurance grants tag and clamps to threshold") {
    DerivedStatRegistry reg;
    registerCoreDerivedStats(reg);
    CoreAttributes core;
    core.dexterity = 20.0f; // threshold 110
    AbilitySystem sys;
    recompute(sys, core, reg);
    GameplayTagRegistry tags;
    tags.registerTag("Status.Poisoned");

    StatusBuildup b;
    addBuildup(b, StatusType::Poison, 120.0f); // ≥ 110
    const auto result = tickBuildup(b, sys, 0.0f, tags);
    REQUIRE(result.triggered.size() == 1);
    CHECK(result.triggered[0] == StatusType::Poison);
    CHECK(b.poison == doctest::Approx(110.0f)); // clamped to threshold; decays from here
    const auto tag = tags.find("Status.Poisoned");
    REQUIRE(tag.has_value());
    CHECK(sys.tags.has(*tag));
}

TEST_CASE("status buildup: higher endurance resists the same buildup") {
    DerivedStatRegistry reg;
    registerCoreDerivedStats(reg);
    CoreAttributes core;
    core.dexterity = 60.0f; // endurancePoison = 100 + 60 × 0.5 = 130
    AbilitySystem sys;
    recompute(sys, core, reg);
    GameplayTagRegistry tags;
    tags.registerTag("Status.Poisoned");

    StatusBuildup b;
    addBuildup(b, StatusType::Poison, 120.0f); // < 130
    CHECK(tickBuildup(b, sys, 0.0f, tags).triggered.empty());
}

TEST_CASE("status buildup: status damage scales with the attribute") {
    CHECK(scaledStatusDamage(100.0f, 20.0f) == doctest::Approx(110.0f));
    CHECK(scaledStatusDamage(100.0f, 10.0f) == doctest::Approx(100.0f));
}

TEST_CASE("status buildup: active status decays at 1%/s and produces poison DoT") {
    DerivedStatRegistry reg;
    registerCoreDerivedStats(reg);
    CoreAttributes core; // default alacrity=6 → vitality = min(2.5, 31) = 2.5%
    AbilitySystem sys;
    recompute(sys, core, reg);
    GameplayTagRegistry tags;
    tags.registerTag("Status.Poisoned");

    // Trigger the status first.
    StatusBuildup b;
    addBuildup(b, StatusType::Poison, 200.0f); // ≥ threshold (103)
    tickBuildup(b, sys, 0.0f, tags);           // trigger: b.poison = threshold ≈ 103
    CHECK(sys.tags.has(*tags.find("Status.Poisoned")));

    // Tick 1s: decay 1% of 103 = 1.03 → ~101.97; DoT ≈ 1*(1-0.025) = 0.975 HP.
    const auto result = tickBuildup(b, sys, 1.0f, tags);
    CHECK(result.triggered.empty());
    CHECK(b.poison < 103.0f);
    const doctest::Approx expectedDot = doctest::Approx(0.975f).epsilon(0.01f);
    CHECK(result.poisonHealthDamage == expectedDot);
}

TEST_CASE("status buildup: tryAddBuildup blocked while status active") {
    DerivedStatRegistry reg;
    registerCoreDerivedStats(reg);
    CoreAttributes core;
    AbilitySystem sys;
    recompute(sys, core, reg);
    GameplayTagRegistry tags;
    tags.registerTag("Status.Poisoned");

    StatusBuildup b;
    addBuildup(b, StatusType::Poison, 200.0f);
    tickBuildup(b, sys, 0.0f, tags); // trigger; tag now active
    const f32 valueAfterTrigger = b.poison;

    tryAddBuildup(b, StatusType::Poison, 50.0f, sys, tags); // blocked
    CHECK(b.poison == doctest::Approx(valueAfterTrigger));
}

TEST_CASE("status buildup: status expires when buildup reaches zero") {
    DerivedStatRegistry reg;
    registerCoreDerivedStats(reg);
    CoreAttributes core;
    AbilitySystem sys;
    recompute(sys, core, reg);
    GameplayTagRegistry tags;
    tags.registerTag("Status.Poisoned");

    StatusBuildup b;
    addBuildup(b, StatusType::Poison, 200.0f);
    tickBuildup(b, sys, 0.0f, tags); // trigger; b.poison = threshold ≈ 103
    // Simulate complete decay: the flat decay (1% threshold/s) reaches exactly 0
    // after threshold/decayPerSec ≈ 100s. Set it directly to verify expiry.
    b.poison = 0.0f;
    tickBuildup(b, sys, 0.0f, tags); // value == 0 → tag removed
    CHECK(b.poison == doctest::Approx(0.0f));
    CHECK(!sys.tags.has(*tags.find("Status.Poisoned")));
}

TEST_CASE("status buildup: bleed triggers burst and immediately resets") {
    DerivedStatRegistry reg;
    registerCoreDerivedStats(reg);
    CoreAttributes core;
    AbilitySystem sys;
    recompute(sys, core, reg);
    GameplayTagRegistry tags;
    tags.registerTag("Status.Bleeding");

    StatusBuildup b;
    addBuildup(b, StatusType::Bleed, 200.0f);
    const auto result = tickBuildup(b, sys, 0.0f, tags);
    REQUIRE(result.triggered.size() == 1);
    CHECK(result.triggered[0] == StatusType::Bleed);
    CHECK(result.bleedBurst);
    CHECK(b.bleed == doctest::Approx(0.0f)); // immediate reset
    // Bleed has no persistent tag — the status should not be active.
    const auto bleedTag = tags.find("Status.Bleeding");
    const bool bleedActive = bleedTag && sys.tags.has(*bleedTag);
    CHECK(!bleedActive);
}

TEST_CASE("status buildup: death triggers instant kill and resets") {
    DerivedStatRegistry reg;
    registerCoreDerivedStats(reg);
    CoreAttributes core;
    AbilitySystem sys;
    recompute(sys, core, reg);
    GameplayTagRegistry tags;
    tags.registerTag("Status.Dying");

    StatusBuildup b;
    addBuildup(b, StatusType::Death, 200.0f);
    const auto result = tickBuildup(b, sys, 0.0f, tags);
    REQUIRE(result.triggered.size() == 1);
    CHECK(result.deathTriggered);
    CHECK(b.death == doctest::Approx(0.0f));
}

TEST_CASE("buildupStatusModifiers: electrocution zeroes essenceRegen mult") {
    DerivedStatRegistry reg;
    registerCoreDerivedStats(reg);
    CoreAttributes core;
    AbilitySystem sys;
    recompute(sys, core, reg);
    GameplayTagRegistry tags;
    tags.registerTag("Status.Electrocuted");
    tags.registerTag("Status.Glaciated");
    StatsTuningForm tuning;

    // No status active: mods unchanged.
    StatModifiers mods;
    buildupStatusModifiers(sys, tags, tuning, mods);
    CHECK(mods.mul.find(attr("essenceRegen")) == mods.mul.end());
    CHECK(mods.mul.find(attr("energyRegen"))  == mods.mul.end());

    // Activate electrocution: essenceRegen suppressed, energyRegen unaffected.
    const auto electrocuted = tags.find("Status.Electrocuted");
    REQUIRE(electrocuted);
    sys.tags.add(*electrocuted, tags);
    mods = {};
    buildupStatusModifiers(sys, tags, tuning, mods);
    REQUIRE(mods.mul.find(attr("essenceRegen")) != mods.mul.end());
    CHECK(mods.mul.at(attr("essenceRegen")) == doctest::Approx(0.0f));
    CHECK(mods.mul.find(attr("energyRegen")) == mods.mul.end());
    sys.tags.remove(*electrocuted, tags);

    // Activate glaciation: energyRegen reduced, essenceRegen unaffected.
    const auto glaciated = tags.find("Status.Glaciated");
    REQUIRE(glaciated);
    sys.tags.add(*glaciated, tags);
    mods = {};
    buildupStatusModifiers(sys, tags, tuning, mods);
    REQUIRE(mods.mul.find(attr("energyRegen")) != mods.mul.end());
    CHECK(mods.mul.at(attr("energyRegen")) == doctest::Approx(tuning.glaciationEnergyRegenMult));
    CHECK(mods.mul.find(attr("essenceRegen")) == mods.mul.end());
}

TEST_CASE("status buildup: ignition/electrocution DoT scale with vitality, not will") {
    // Regression for the copy-paste bug where ignition/electrocution reduced
    // their ongoing damage by `will` (ego) instead of `vitality` (alacrity).
    // docs/STATS.md §145/§183: vitality reduces a status's ongoing damage.
    auto elementalDot = [](StatusType type, const char* tag, f32 alacrity, f32 ego) {
        DerivedStatRegistry reg;
        registerCoreDerivedStats(reg);
        CoreAttributes core;
        core.strength = core.constitution = core.grace = 20.0f; // maxHealth = 300
        core.alacrity = alacrity; // → vitality
        core.ego = ego;           // → will
        AbilitySystem sys;
        recompute(sys, core, reg);
        GameplayTagRegistry tags;
        tags.registerTag(tag);
        StatusBuildup b;
        addBuildup(b, type, 500.0f);
        tickBuildup(b, sys, 0.0f, tags);        // trigger → status active
        return tickBuildup(b, sys, 1.0f, tags); // ongoing DoT
    };

    // Ignition drains health (maxHealth = str/con/grace, independent of ego),
    // so ego is a clean "will" knob here: cranking it must not change the DoT.
    const f32 ignBase = elementalDot(StatusType::Ignition, "Status.Ignited", 6.0f, 6.0f).ignitionHealthDamage;
    const f32 ignWill = elementalDot(StatusType::Ignition, "Status.Ignited", 6.0f, 90.0f).ignitionHealthDamage;
    const f32 ignVit  = elementalDot(StatusType::Ignition, "Status.Ignited", 90.0f, 6.0f).ignitionHealthDamage;
    CHECK(ignBase > 0.0f);
    CHECK(ignWill == doctest::Approx(ignBase)); // high will (ego) must NOT reduce it
    CHECK(ignVit  <  ignBase);                  // high vitality (alacrity) reduces it

    // Electrocution drains essence. maxEssence depends on ego, so ego is NOT a
    // clean will knob here (ignition covers the "not will" case). We assert the
    // positive property: raising vitality (alacrity — feeds neither maxEssence
    // nor maxHealth) reduces the DoT.
    const f32 elBase = elementalDot(StatusType::Electrocution, "Status.Electrocuted", 6.0f, 6.0f).electrocutionEssenceDamage;
    const f32 elVit  = elementalDot(StatusType::Electrocution, "Status.Electrocuted", 90.0f, 6.0f).electrocutionEssenceDamage;
    CHECK(elBase > 0.0f);
    CHECK(elVit  <  elBase);
}

// --- applyBuildupResult: the ONE shared consequence path ----------------------------

#include "gameplay/combat/Combat.hpp"   // updateLifeState
#include "gameplay/stats/GameTime.hpp"  // applyBuildupResult, GameTimeTickArgs

namespace {

// Full character bundle for applyBuildupResult (a plain fixture — the args
// struct wants every component the game-time path can touch).
struct BuildupFixture {
    DerivedStatRegistry reg;
    GameplayTagRegistry tags;
    StatsTuningForm tuning;
    CoreAttributes core;
    AttributeSet vitals;
    AbilitySystem sys;
    CombatState combat;
    StatusBuildup buildup;
    Survival survival;
    Injuries injuries;
    Resonance resonance;
    ResonanceDecays decays;
    StatModifiers mods;

    BuildupFixture() {
        registerCoreDerivedStats(reg);
        tags.registerTag("State.Dead");
        tags.registerTag("State.Staggered");
        tags.registerTag("State.Paralyzed");
        recomputeStats(core, vitals, resonance, sys, reg, &mods);
        vitals.health = currentValueOf(sys, attr("maxHealth"));
        combat.posture = currentValueOf(sys, attr("maxPosture"));
        recomputeStats(core, vitals, resonance, sys, reg, &mods);
    }

    GameTimeTickArgs args() {
        return { core,     vitals,    sys,  combat, buildup, survival,
                 injuries, resonance, decays, reg,  tags,    tuning };
    }
};

} // namespace

TEST_CASE("buildup result: lethal zeroing writes base health AND State.Dead "
          "(no resurrection)") {
    BuildupFixture f;
    GameTimeTickArgs a = f.args();

    BuildupTickResult br;
    br.deathTriggered = true;
    CHECK(applyBuildupResult(a, br, f.mods));
    // The old real-time path only added the tag: health stayed > 0, so the
    // next life-state sync resurrected the actor (and it reloaded alive).
    CHECK(f.vitals.health == 0.0f);
    CHECK(f.sys.tags.has(*f.tags.find("State.Dead")));
    updateLifeState(f.sys, f.tags); // a later sync must keep the corpse dead
    CHECK(f.sys.tags.has(*f.tags.find("State.Dead")));
}

TEST_CASE("buildup result: a DoT draining health to zero kills immediately") {
    BuildupFixture f;
    f.vitals.health = 0.5f;
    GameTimeTickArgs a = f.args();

    BuildupTickResult br;
    br.poisonHealthDamage = 1.0f;
    CHECK(applyBuildupResult(a, br, f.mods));
    CHECK(f.vitals.health == 0.0f);
    // The death tag lands THIS tick (the current overlay is refreshed before
    // the sync), not a full tick later.
    CHECK(f.sys.tags.has(*f.tags.find("State.Dead")));
}

TEST_CASE("buildup result: electrocution drains posture AND staggers "
          "(was real-time only)") {
    BuildupFixture f;
    GameTimeTickArgs a = f.args();
    const f32 postureBefore = f.combat.posture;
    REQUIRE(postureBefore > 0.0f);

    BuildupTickResult br;
    br.electrocutionTriggered = true;
    CHECK(!applyBuildupResult(a, br, f.mods));
    CHECK(f.combat.posture < postureBefore);
    CHECK(f.combat.staggerSeconds > 0.0f);
    CHECK(f.sys.tags.has(*f.tags.find("State.Staggered")));
    CHECK(!f.sys.tags.has(*f.tags.find("State.Dead")));
}

TEST_CASE("buildup result: glaciation paralyses on both paths") {
    BuildupFixture f;
    GameTimeTickArgs a = f.args();

    BuildupTickResult br;
    br.glaciationTriggered = true;
    CHECK(!applyBuildupResult(a, br, f.mods));
    CHECK(f.combat.paralysisSeconds ==
          doctest::Approx(f.tuning.glaciationParalysisDuration));
    CHECK(f.sys.tags.has(*f.tags.find("State.Paralyzed")));
}
