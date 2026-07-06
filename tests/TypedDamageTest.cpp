#include <doctest/doctest.h>

#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/GameplayTags.hpp"
#include "gameplay/stats/CharacterStats.hpp"
#include "gameplay/stats/CoreAttributes.hpp"
#include "gameplay/stats/Damage.hpp"

using namespace gameplay;

namespace {
// All nine attributes at 20 for clean mitigation math:
//   defense = armor* = resist* = 10, will = 6, maxPosture = 70, maxHealth = 300.
struct Fixture {
    CoreAttributes core;
    AttributeSet vitals;
    AbilitySystem system;
    CombatState combat;
    DerivedStatRegistry derived;
    GameplayTagRegistry tags;

    Fixture() {
        core.strength = core.constitution = core.grace = 20.0f;
        core.dexterity = core.alacrity = core.perception = 20.0f;
        core.charisma = core.ego = core.insight = 20.0f;
        vitals.health = 300.0f;
        registerCoreDerivedStats(derived);
        tags.registerTag("State.Dead");
        tags.registerTag("State.Staggered");
        recomputeStats(core, vitals, system, derived, nullptr);
        combat.posture = currentValueOf(system, attr("maxPosture")); // 70
    }
    StatBlock block() { return StatBlock { core, vitals, system, combat }; }
};

DamageResult hit(Fixture& f, DamageType type, f32 amount, f32 posture = 0.0f) {
    StatBlock b = f.block();
    return applyDamage(b, DamageEvent { { { type, amount } }, posture }, f.tags,
                       f.derived);
}
} // namespace

TEST_CASE("typed damage: physical channel applies flat defense then armor %") {
    Fixture f;
    // slash 100: flat min(defense 10, 45% of 100 = 45) = 10 → 90 → armor 10% → 81.
    const DamageResult r = hit(f, DamageType::Slash, 100.0f);
    CHECK(r.healthDamage == doctest::Approx(81.0f));
    CHECK(currentValueOf(f.system, attr("health")) == doctest::Approx(219.0f));
}

TEST_CASE("typed damage: the flat reduction is capped at 25 + attr % of the hit") {
    Fixture f;
    // slash 10: flat min(defense 10, 45% of 10 = 4.5) = 4.5 (cap binds) → 5.5 → ×0.9.
    const DamageResult r = hit(f, DamageType::Slash, 10.0f);
    CHECK(r.healthDamage == doctest::Approx(4.95f));
}

TEST_CASE("typed damage: elemental channel uses will then resistance %") {
    Fixture f;
    // fire 100: flat min(will 6, 45% of 100) = 6 → 94 → resistFire 10% → 84.6.
    const DamageResult r = hit(f, DamageType::Fire, 100.0f);
    CHECK(r.healthDamage == doctest::Approx(84.6f));
}

TEST_CASE("typed damage: a new element uses will then its own resistance %") {
    Fixture f;
    // holy 100: flat min(will 6, 45) = 6 → 94 → resistHoly (0.5·charisma = 10) 10% → 84.6.
    const DamageResult r = hit(f, DamageType::Holy, 100.0f);
    CHECK(r.healthDamage == doctest::Approx(84.6f));
}

TEST_CASE("typed damage: negative resistance amplifies the hit (vulnerability)") {
    Fixture f;
    // Iron-like conductive armor: resistLightning 10 + (-30) = -20% → ×1.2.
    StatModifiers armor;
    armor.add[attr("resistLightning")] += -30.0f;
    recomputeStats(f.core, f.vitals, f.system, f.derived, &armor); // fold into current
    StatBlock b = f.block();
    const DamageResult r =
        applyDamage(b, DamageEvent { { { DamageType::Lightning, 100.0f } }, 0.0f },
                    f.tags, f.derived, &armor);
    // flat min(will 6, 45) = 6 → 94 → ×(1 − (−0.20)) = 94 × 1.2 = 112.8.
    CHECK(r.healthDamage == doctest::Approx(112.8f));
}

TEST_CASE("typed damage: channels sum onto health") {
    Fixture f;
    StatBlock b = f.block();
    const DamageResult r = applyDamage(
        b,
        DamageEvent { { { DamageType::Slash, 100.0f }, { DamageType::Fire, 100.0f } },
                      0.0f },
        f.tags, f.derived);
    CHECK(r.healthDamage == doctest::Approx(81.0f + 84.6f)); // 165.6
    CHECK(currentValueOf(f.system, attr("health")) == doctest::Approx(300.0f - 165.6f));
}

TEST_CASE("posture: a break staggers and opens the critical window (C2)") {
    Fixture f;
    f.tags.registerTag("State.CriticalWeakness");
    const auto staggered = f.tags.find("State.Staggered");
    const auto weakness = f.tags.find("State.CriticalWeakness");
    REQUIRE(staggered.has_value());
    REQUIRE(weakness.has_value());

    StatBlock b = f.block();
    const DamageResult r = applyDamage(
        b, DamageEvent { { { DamageType::Blunt, 10.0f } }, 80.0f }, f.tags, f.derived);
    CHECK(r.staggered);
    CHECK(f.system.tags.has(*staggered));
    CHECK(f.system.tags.has(*weakness));
    CHECK(f.combat.staggerSeconds == doctest::Approx(1.5f));
    CHECK(f.combat.critWindowSeconds == doctest::Approx(5.0f));
    CHECK(f.combat.posture == doctest::Approx(0.0f)); // sits at 0 all window

    updateStagger(f.combat, f.system, 1.0f, f.tags); // not yet elapsed
    CHECK(f.system.tags.has(*staggered));
    updateStagger(f.combat, f.system, 1.0f, f.tags); // elapsed
    CHECK_FALSE(f.system.tags.has(*staggered));

    updateCritWindow(f.combat, f.system, 4.0f, f.tags); // still open
    CHECK(f.system.tags.has(*weakness));
    CHECK(f.combat.posture == doctest::Approx(0.0f));
    updateCritWindow(f.combat, f.system, 1.5f, f.tags); // expires
    CHECK_FALSE(f.system.tags.has(*weakness));
    CHECK(f.combat.posture == doctest::Approx(70.0f)); // refilled on expiry
}

TEST_CASE("posture: a heavy non-breaking hit grants a short Shaken (C2)") {
    Fixture f;
    f.tags.registerTag("State.Shaken");
    const auto shaken = f.tags.find("State.Shaken");
    REQUIRE(shaken.has_value());

    // Threshold = (15 + constitution 20)% of maxPosture 70 = 24.5.
    StatBlock b = f.block();
    applyDamage(b, DamageEvent { {}, 20.0f }, f.tags, f.derived);
    CHECK_FALSE(f.system.tags.has(*shaken)); // below threshold

    applyDamage(b, DamageEvent { {}, 30.0f }, f.tags, f.derived);
    CHECK(f.system.tags.has(*shaken));
    CHECK(f.combat.shakenSeconds == doctest::Approx(0.6f));
    CHECK(f.combat.posture == doctest::Approx(20.0f)); // 70 − 20 − 30, no break

    updateShaken(f.combat, f.system, 1.0f, f.tags);
    CHECK_FALSE(f.system.tags.has(*shaken));
}
