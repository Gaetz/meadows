#include <doctest/doctest.h>

#include "engine/ecs/World.hpp"
#include "world/scene/Components.hpp"
#include "world/scene/Movement.hpp"

using namespace world;

TEST_CASE("movement: integrates transform by velocity over dt") {
    ecs::World world;
    registerSceneComponents(world);

    ecs::Entity entity = world.create();
    Transform transform;
    transform.position = { 1.0f, 2.0f, 0.0f };
    entity.set<Transform>(transform);
    entity.set<Velocity>({ Vec3 { 4.0f, -2.0f, 0.0f } });

    applyMovement(world, 0.5f);

    CHECK(entity.get<Transform>().position.x == doctest::Approx(3.0f));  // 1 + 4*0.5
    CHECK(entity.get<Transform>().position.y == doctest::Approx(1.0f));  // 2 + -2*0.5
}

TEST_CASE("movement: entities without a Velocity are not moved") {
    ecs::World world;
    registerSceneComponents(world);

    ecs::Entity entity = world.create();
    Transform transform;
    transform.position = { 5.0f, 5.0f, 0.0f };
    entity.set<Transform>(transform);

    applyMovement(world, 1.0f);

    CHECK(entity.get<Transform>().position.x == 5.0f);
    CHECK(entity.get<Transform>().position.y == 5.0f);
}
