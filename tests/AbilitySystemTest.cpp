#include <doctest/doctest.h>

#include "engine/ecs/World.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/Attributes.hpp"

using namespace gameplay;

TEST_CASE("attributes: base values read/written by reflection-addressed id") {
    AttributeSet set;
    CHECK(baseValueOf(set, attr("health")) == 100.0f);
    CHECK(baseValueOf(set, attr("essence")) == 50.0f);
    CHECK_FALSE(baseValueOf(set, attr("doesNotExist")).has_value());

    CHECK(setBaseValue(set, attr("maxHealth"), 150.0f));
    CHECK(baseValueOf(set, attr("maxHealth")) == 150.0f);
    CHECK_FALSE(setBaseValue(set, attr("doesNotExist"), 1.0f));
}

TEST_CASE("attributes: current overlay is seeded from base, then independent") {
    AttributeSet set;
    setBaseValue(set, attr("health"), 80.0f);

    AbilitySystem system;
    initializeCurrent(system, set);
    CHECK(currentValueOf(system, attr("health")) == 80.0f);
    CHECK(currentValueOf(system, attr("essence")) == 50.0f);
    CHECK(currentValueOf(system, attr("doesNotExist")) == 0.0f);

    system.current[attr("health")] = 30.0f; // poke the overlay directly (test only)
    CHECK(currentValueOf(system, attr("health")) == 30.0f);
    CHECK(baseValueOf(set, attr("health")) == 80.0f); // base untouched
}

TEST_CASE("ability system: GAS components register and attach to an entity") {
    ecs::World world;
    registerGameplayComponents(world);

    ecs::Entity actor = world.create();
    actor.set<AttributeSet>({});
    actor.set<AbilitySystem>({});

    REQUIRE(actor.try_get<AttributeSet>() != nullptr);
    REQUIRE(actor.try_get<AbilitySystem>() != nullptr);
    CHECK(actor.get<AttributeSet>().health == 100.0f);
}
