#include <doctest/doctest.h>
#include "gameplay/actors/Swimming.hpp"

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

TEST_CASE("swimming transitions: deep water swims, shores and surfaces "
          "ground (P0 D2b)") {
    using gameplay::decideMoveMode;
    using gameplay::MoveMode;
    const f32 head = 1.7f;

    // Dry land / interiors: always Ground.
    CHECK(decideMoveMode(MoveMode::Ground, std::nullopt, 10.0f, head,
                         true) == MoveMode::Ground);
    CHECK(decideMoveMode(MoveMode::Swim, std::nullopt, 10.0f, head,
                         false) == MoveMode::Ground);

    // Wading in: the head must sink 0.3 under the surface to swim.
    // Surface at 10: feet at 8.1 -> head 9.8 > 9.7: still wading.
    CHECK(decideMoveMode(MoveMode::Ground, 10.0f, 8.1f, head, true) ==
          MoveMode::Ground);
    // Feet at 7.9 -> head 9.6 < 9.7: swimming.
    CHECK(decideMoveMode(MoveMode::Ground, 10.0f, 7.9f, head, false) ==
          MoveMode::Swim);

    // Hysteresis: a swimmer just under the surface KEEPS swimming...
    CHECK(decideMoveMode(MoveMode::Swim, 10.0f, 8.2f, head, false) ==
          MoveMode::Swim);
    // ...grounds when the head clears the surface...
    CHECK(decideMoveMode(MoveMode::Swim, 10.0f, 8.4f, head, false) ==
          MoveMode::Ground);
    // ...or when the feet find ground in shallow water (wading out:
    // depth 1.0 < 0.65 x 1.7).
    CHECK(decideMoveMode(MoveMode::Swim, 10.0f, 9.0f, head, true) ==
          MoveMode::Ground);
    // Deep bottom contact is NOT an exit (walking on a lake bed).
    CHECK(decideMoveMode(MoveMode::Swim, 10.0f, 7.0f, head, true) ==
          MoveMode::Swim);
}
