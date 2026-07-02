#include <doctest/doctest.h>

#include <memory>

#include "data/forms/FormDatabase.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/Attributes.hpp"
#include "gameplay/ability/GameplayAbility.hpp"
#include "gameplay/ability/GameplayEffects.hpp"

using core::Guid;
using namespace gameplay;

namespace {

const Guid kCost = *Guid::fromString("c0000000-0000-4000-8000-000000000001");
const Guid kCooldown = *Guid::fromString("c0000000-0000-4000-8000-000000000002");
const Guid kDamage = *Guid::fromString("c0000000-0000-4000-8000-000000000003");
const Guid kAbility = *Guid::fromString("ab000000-0000-4000-8000-000000000001");

// A "slash": costs 20 energy, deals 30 damage, 5s cooldown.
struct Fixture {
    data::FormDatabase db;
    GameplayTagRegistry registry;

    AttributeSet casterSet;
    AbilitySystem casterSystem;
    AttributeSet targetSet;
    AbilitySystem targetSystem;

    Fixture(const char* costAttr = "energy", const char* costPolicy = "") {
        registry.registerTag("Cooldown.Slash");
        registry.registerTag("Status.Stunned");

        addEffect(kCost, costAttr, "add", -20.0f, "instant", 0.0f, "");
        addEffect(kCooldown, "", "add", 0.0f, "duration", 5.0f, "Cooldown.Slash");
        addEffect(kDamage, "damage", "add", 30.0f, "instant", 0.0f, "");

        auto ability = std::make_unique<AbilityForm>();
        ability->id = kAbility;
        ability->blockedTag = "Status.Stunned";
        ability->cost = kCost;
        ability->cooldown = kCooldown;
        ability->effect = kDamage;
        ability->costPolicy = costPolicy;
        db.add(std::move(ability), AbilityForm::staticTypeInfo());

        initializeCurrent(casterSystem, casterSet);
        initializeCurrent(targetSystem, targetSet);
    }

    void addEffect(const Guid& id, const char* attribute, const char* op,
                   f32 magnitude, const char* duration, f32 durationSeconds,
                   const char* grantedTag) {
        auto e = std::make_unique<EffectForm>();
        e->id = id;
        e->attribute = attribute;
        e->op = op;
        e->magnitude = magnitude;
        e->duration = duration;
        e->durationSeconds = durationSeconds;
        e->grantedTag = grantedTag;
        db.add(std::move(e), EffectForm::staticTypeInfo());
    }

    const AbilityForm& ability() const {
        return *db.find<AbilityForm>(kAbility);
    }

    bool activate() {
        return tryActivate(ability(), casterSet, casterSystem, targetSet,
                           targetSystem, { db, registry });
    }
};

} // namespace

TEST_CASE("ability: a successful activation pays cost, applies effect, sets cooldown") {
    Fixture f;
    CHECK(f.activate());

    CHECK(baseValueOf(f.casterSet, attr("energy")) == 80.0f);  // -20 cost
    CHECK(baseValueOf(f.targetSet, attr("health")) == 70.0f);  // -30 damage
    CHECK(f.casterSystem.tags.has(*f.registry.find("Cooldown.Slash")));
}

TEST_CASE("ability: cannot reactivate while on cooldown") {
    Fixture f;
    REQUIRE(f.activate());
    CHECK_FALSE(f.activate()); // cooldown tag present

    CHECK(baseValueOf(f.casterSet, attr("energy")) == 80.0f); // no second cost
    CHECK(baseValueOf(f.targetSet, attr("health")) == 70.0f); // no second hit
}

TEST_CASE("ability: a blocked activation tag prevents activation") {
    Fixture f;
    f.casterSystem.tags.add(*f.registry.find("Status.Stunned"), f.registry);

    CHECK_FALSE(f.activate());
    CHECK(baseValueOf(f.targetSet, attr("health")) == 100.0f); // untouched
    CHECK(baseValueOf(f.casterSet, attr("energy")) == 100.0f);
}

TEST_CASE("ability: energy cost is permissive — activates with any reserve") {
    Fixture f; // energy cost, default policy → permissive
    setBaseValue(f.casterSet, attr("energy"), 10.0f); // less than the 20 cost
    initializeCurrent(f.casterSystem, f.casterSet);

    CHECK(f.activate()); // permissive: 10 > 0 is enough
    CHECK(baseValueOf(f.casterSet, attr("energy")) == 0.0f); // overdraw clamps to 0
    CHECK(baseValueOf(f.targetSet, attr("health")) == 70.0f);
}

TEST_CASE("ability: permissive cost still blocks at zero reserve") {
    Fixture f;
    setBaseValue(f.casterSet, attr("energy"), 0.0f);
    initializeCurrent(f.casterSystem, f.casterSet);

    CHECK_FALSE(f.activate()); // empty → blocked
    CHECK(baseValueOf(f.targetSet, attr("health")) == 100.0f);
    CHECK(baseValueOf(f.casterSet, attr("energy")) == 0.0f);
}

TEST_CASE("ability: magic (essence) cost is strict — full cost required") {
    Fixture f("essence"); // non-energy resource → strict by default
    setBaseValue(f.casterSet, attr("essence"), 10.0f); // less than the 20 cost
    initializeCurrent(f.casterSystem, f.casterSet);

    CHECK_FALSE(f.activate()); // strict: 10 < 20 → blocked
    CHECK(baseValueOf(f.targetSet, attr("health")) == 100.0f);
    CHECK(baseValueOf(f.casterSet, attr("essence")) == 10.0f);
}

TEST_CASE("ability: costPolicy overrides the resource default") {
    Fixture f("energy", "strict"); // force strict on an energy cost
    setBaseValue(f.casterSet, attr("energy"), 10.0f);
    initializeCurrent(f.casterSystem, f.casterSet);

    CHECK_FALSE(f.activate()); // strict override: 10 < 20 → blocked
    CHECK(baseValueOf(f.casterSet, attr("energy")) == 10.0f);
}

TEST_CASE("ability: reactivates once the cooldown expires") {
    Fixture f;
    REQUIRE(f.activate());

    tickEffects(f.casterSet, f.casterSystem, 5.0f, f.registry); // cooldown elapses
    CHECK_FALSE(f.casterSystem.tags.has(*f.registry.find("Cooldown.Slash")));

    CHECK(f.activate());
    CHECK(baseValueOf(f.targetSet, attr("health")) == 40.0f); // two hits of 30
    CHECK(baseValueOf(f.casterSet, attr("energy")) == 60.0f); // two costs of 20
}
