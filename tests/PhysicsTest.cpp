#include <doctest/doctest.h>

#include "engine/physics/Physics.hpp"

// H3: the Jolt seam works headless — a capsule character falls onto a
// static floor, lands, and walks; rays hit what they should.

TEST_CASE("a character capsule falls onto a static box and rests on it") {
    phys::PhysicsWorld world;
    // Floor: top face at y = 0.
    world.addStaticBox({ 50.0f, 1.0f, 50.0f }, { 0.0f, -1.0f, 0.0f });

    phys::CharacterBody character { world, 0.35f, 1.8f,
                                    { 0.0f, 3.0f, 0.0f } };
    CHECK_FALSE(character.onGround());

    constexpr f32 dt = 1.0f / 60.0f;
    for (int i = 0; i < 180; ++i) { // 3 seconds: plenty to land
        character.move({ 0.0f, 0.0f, 0.0f }, dt);
        world.tick(dt);
    }
    CHECK(character.onGround());
    CHECK(character.position().y == doctest::Approx(0.0f).epsilon(0.05));

    // Walk forward: horizontal motion, still grounded.
    for (int i = 0; i < 60; ++i) {
        character.move({ 2.0f, 0.0f, 0.0f }, dt);
        world.tick(dt);
    }
    CHECK(character.position().x == doctest::Approx(2.0f).epsilon(0.15));
    CHECK(character.onGround());

    // Jump: leaves the ground, comes back.
    character.jump(5.0f);
    character.move({ 0.0f, 0.0f, 0.0f }, dt);
    CHECK_FALSE(character.onGround());
    for (int i = 0; i < 180; ++i) {
        character.move({ 0.0f, 0.0f, 0.0f }, dt);
        world.tick(dt);
    }
    CHECK(character.onGround());
}

TEST_CASE("raycasts hit static geometry and miss empty space") {
    phys::PhysicsWorld world;
    const phys::BodyId floor =
        world.addStaticBox({ 10.0f, 1.0f, 10.0f }, { 0.0f, -1.0f, 0.0f });

    const phys::RayHit down =
        world.rayCast({ 0.0f, 5.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 20.0f);
    REQUIRE(down.hit);
    CHECK(down.distance == doctest::Approx(5.0f).epsilon(0.01));
    CHECK(down.position.y == doctest::Approx(0.0f).epsilon(0.01));
    CHECK(down.normal.y == doctest::Approx(1.0f).epsilon(0.01));
    CHECK(down.body == floor);

    const phys::RayHit up =
        world.rayCast({ 0.0f, 5.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, 20.0f);
    CHECK_FALSE(up.hit);

    world.removeBody(floor);
    const phys::RayHit afterRemove =
        world.rayCast({ 0.0f, 5.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 20.0f);
    CHECK_FALSE(afterRemove.hit);
}
