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

TEST_CASE("a capsule lands on a height field and climbs its slope (B4)") {
    phys::PhysicsWorld world;
    // 64x64 samples, 1 m apart: a plane at y = 5 rising along +X at 20 %
    // past x = 32 (gentle slope, well under the 50° walkable limit).
    constexpr u32 n = 64;
    vector<f32> samples(static_cast<size_t>(n) * n);
    for (u32 row = 0; row < n; ++row) {
        for (u32 col = 0; col < n; ++col) {
            const f32 x = static_cast<f32>(col);
            samples[row * n + col] =
                5.0f + (x > 32.0f ? (x - 32.0f) * 0.2f : 0.0f);
        }
    }
    const phys::BodyId field =
        world.addHeightField(samples.data(), n, { 0.0f, 0.0f, 0.0f }, 1.0f);
    REQUIRE(field != 0);

    // Rays: flat part, then the slope.
    const phys::RayHit flat =
        world.rayCast({ 10.0f, 20.0f, 10.0f }, { 0.0f, -1.0f, 0.0f }, 40.0f);
    REQUIRE(flat.hit);
    CHECK(flat.position.y == doctest::Approx(5.0f).epsilon(0.01));
    const phys::RayHit slope =
        world.rayCast({ 42.0f, 20.0f, 10.0f }, { 0.0f, -1.0f, 0.0f }, 40.0f);
    REQUIRE(slope.hit);
    CHECK(slope.position.y == doctest::Approx(7.0f).epsilon(0.01));

    // A capsule dropped over the flat part rests at field height...
    phys::CharacterBody character { world, 0.35f, 1.8f,
                                    { 10.0f, 9.0f, 10.0f } };
    constexpr f32 dt = 1.0f / 60.0f;
    for (int i = 0; i < 180; ++i) {
        character.move({ 0.0f, 0.0f, 0.0f }, dt);
        world.tick(dt);
    }
    CHECK(character.onGround());
    CHECK(character.position().y == doctest::Approx(5.0f).epsilon(0.05));

    // ...then walks up the slope, staying grounded and gaining height.
    for (int i = 0; i < 600; ++i) { // 10 s at 3 m/s -> well onto the slope
        character.move({ 3.0f, 0.0f, 0.0f }, dt);
        world.tick(dt);
    }
    CHECK(character.onGround());
    CHECK(character.position().x > 35.0f);
    CHECK(character.position().y >
          5.0f + (character.position().x - 32.0f) * 0.2f - 0.5f);
}

TEST_CASE("a static triangle mesh blocks a capsule and rays (ch.2 B2)") {
    phys::PhysicsWorld world;
    // A 10x10 m quad at y = 2, two triangles, scaled x2 through the API.
    const Vec3 vertices[] = { { -5.0f, 2.0f, -5.0f },
                              { 5.0f, 2.0f, -5.0f },
                              { 5.0f, 2.0f, 5.0f },
                              { -5.0f, 2.0f, 5.0f } };
    const u32 indices[] = { 0, 1, 2, 0, 2, 3 };
    const phys::BodyId mesh = world.addStaticMesh(
        vertices, 4, indices, 6, { 0.0f, 0.0f, 0.0f },
        { 1.0f, 0.0f, 0.0f, 0.0f }, { 2.0f, 1.0f, 2.0f });
    REQUIRE(mesh != 0);

    const phys::RayHit hit =
        world.rayCast({ 8.0f, 10.0f, 8.0f }, { 0.0f, -1.0f, 0.0f }, 20.0f);
    REQUIRE(hit.hit); // x=8 is inside the SCALED quad (±10)
    CHECK(hit.position.y == doctest::Approx(2.0f).epsilon(0.01));

    phys::CharacterBody capsule { world, 0.35f, 1.8f, { 0.0f, 6.0f, 0.0f } };
    constexpr f32 dt = 1.0f / 60.0f;
    for (int i = 0; i < 180; ++i) {
        capsule.move({ 0.0f, 0.0f, 0.0f }, dt);
        world.tick(dt);
    }
    CHECK(capsule.onGround());
    CHECK(capsule.position().y == doctest::Approx(2.0f).epsilon(0.05));
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

// The melee/projectile sweep primitive.
TEST_CASE("sphere casts sweep into geometry a ray would miss") {
    phys::PhysicsWorld world;
    const phys::BodyId post =
        world.addStaticBox({ 0.2f, 2.0f, 0.2f }, { 0.0f, 2.0f, 0.0f });

    // Straight at the post: the swept sphere touches ~radius short of
    // the surface a ray reaches.
    const phys::RayHit swept = world.sphereCast(
        { 0.0f, 2.0f, 5.0f }, { 0.0f, 0.0f, -1.0f }, 10.0f, 0.5f);
    REQUIRE(swept.hit);
    CHECK(swept.body == post);
    CHECK(swept.distance == doctest::Approx(4.3f).epsilon(0.02));
    const phys::RayHit ray = world.rayCast({ 0.0f, 2.0f, 5.0f },
                                           { 0.0f, 0.0f, -1.0f }, 10.0f);
    REQUIRE(ray.hit);
    CHECK(swept.distance < ray.distance);
    // The contact sits on the struck surface, facing the sweep.
    CHECK(swept.position.z == doctest::Approx(0.2f).epsilon(0.05));
    CHECK(swept.normal.z == doctest::Approx(1.0f).epsilon(0.05));

    // Off to the side: a RAY misses the post, the fat sweep clips it —
    // exactly the melee-arc forgiveness the hit windows want (A4).
    const phys::RayHit grazeRay = world.rayCast(
        { 0.55f, 2.0f, 5.0f }, { 0.0f, 0.0f, -1.0f }, 10.0f);
    CHECK_FALSE(grazeRay.hit);
    const phys::RayHit graze = world.sphereCast(
        { 0.55f, 2.0f, 5.0f }, { 0.0f, 0.0f, -1.0f }, 10.0f, 0.5f);
    CHECK(graze.hit);

    // Far to the side: even the sweep misses.
    const phys::RayHit wide = world.sphereCast(
        { 2.0f, 2.0f, 5.0f }, { 0.0f, 0.0f, -1.0f }, 10.0f, 0.5f);
    CHECK_FALSE(wide.hit);
}
