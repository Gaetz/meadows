#include <doctest/doctest.h>

#include <memory>

#include "data/forms/FormDatabase.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/Attributes.hpp"
#include "gameplay/ability/GameplayAbility.hpp"
#include "gameplay/ability/GameplayEffects.hpp"
#include "gameplay/combat/Combat.hpp"

using core::Guid;
using namespace gameplay;

TEST_CASE("combat: life state tracks current health crossing zero") {
    GameplayTagRegistry registry;
    registry.registerTag("State.Dead");
    const GameplayTag dead = *registry.find("State.Dead");

    AttributeSet set;
    AbilitySystem system;
    initializeCurrent(system, set);

    updateLifeState(system, registry);
    CHECK_FALSE(system.tags.has(dead));

    EffectForm lethal;
    lethal.attribute = "damage";
    lethal.op = "add";
    lethal.magnitude = 100.0f;
    lethal.duration = "instant";
    applyEffect(set, system, lethal, registry);
    updateLifeState(system, registry);
    CHECK(system.tags.has(dead)); // health hit 0

    EffectForm revive;
    revive.attribute = "health";
    revive.op = "add";
    revive.magnitude = 25.0f;
    revive.duration = "instant";
    applyEffect(set, system, revive, registry);
    updateLifeState(system, registry);
    CHECK_FALSE(system.tags.has(dead)); // alive again
}

TEST_CASE("combat: an attack ability damages the target and can kill it") {
    const Guid damageId = *Guid::fromString("e0000000-0000-4000-8000-000000000001");
    const Guid attackId = *Guid::fromString("ab000000-0000-4000-8000-0000000000aa");

    data::FormDatabase db;
    GameplayTagRegistry registry;
    registry.registerTag("State.Dead");

    auto damage = std::make_unique<EffectForm>();
    damage->id = damageId;
    damage->attribute = "damage";
    damage->op = "add";
    damage->magnitude = 60.0f;
    damage->duration = "instant";
    db.add(std::move(damage), EffectForm::staticTypeInfo());

    auto attack = std::make_unique<AbilityForm>();
    attack->id = attackId;
    attack->effect = damageId;
    db.add(std::move(attack), AbilityForm::staticTypeInfo());

    AttributeSet attackerSet;
    AbilitySystem attackerSystem;
    initializeCurrent(attackerSystem, attackerSet);
    AttributeSet targetSet;
    AbilitySystem targetSystem;
    initializeCurrent(targetSystem, targetSet);

    const AbilityForm& ability = *db.find<AbilityForm>(attackId);
    const AbilityContext ctx { db, registry };
    const GameplayTag dead = *registry.find("State.Dead");

    CHECK(performAttack(ability, attackerSet, attackerSystem, targetSet,
                        targetSystem, ctx));
    CHECK(baseValueOf(targetSet, attr("health")) == 40.0f);
    CHECK_FALSE(targetSystem.tags.has(dead));

    CHECK(performAttack(ability, attackerSet, attackerSystem, targetSet,
                        targetSystem, ctx));
    CHECK(baseValueOf(targetSet, attr("health")) == 0.0f);
    CHECK(targetSystem.tags.has(dead)); // two 60-damage hits killed it
}
