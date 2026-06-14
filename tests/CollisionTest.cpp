#include <doctest/doctest.h>

#include "engine/ecs/World.hpp"
#include "world/scene/Collision.hpp"
#include "world/scene/Components.hpp"

using namespace world;

TEST_CASE("collision: AABB overlap test") {
    CHECK(aabbOverlap({ 0, 0 }, { 1, 1 }, { 1, 0 }, { 1, 1 }));   // overlapping
    CHECK_FALSE(aabbOverlap({ 0, 0 }, { 1, 1 }, { 3, 0 }, { 1, 1 })); // apart
}

TEST_CASE("collision: minimum translation pushes along the least-penetration axis") {
    // A at (0.5,0) vs B at origin, both half (1,1): x-overlap 1.5 < y-overlap 2.
    const Vec2 mtv = minimumTranslation({ 0.5f, 0.0f }, { 1, 1 }, { 0, 0 }, { 1, 1 });
    CHECK(mtv.y == 0.0f);
    CHECK(mtv.x == doctest::Approx(1.5f)); // push +x (A is to the right)
}

TEST_CASE("collision: a dynamic entity is pushed out of a solid") {
    ecs::World world;
    registerSceneComponents(world);

    ecs::Entity wall = world.create();
    wall.set<Transform>({ Vec3 { 0, 0, 0 } });
    wall.set<Collider>({ Vec2 { 1, 1 }, false });

    ecs::Entity mover = world.create();
    mover.set<Transform>({ Vec3 { 1.5f, 0, 0 } }); // overlapping the wall
    mover.set<Collider>({ Vec2 { 1, 1 }, false });
    mover.set<Velocity>({});

    resolveCollisions(world);
    CHECK(mover.get<Transform>().position.x == doctest::Approx(2.0f)); // pushed to edge
    CHECK(wall.get<Transform>().position.x == 0.0f);                   // static unmoved
}

TEST_CASE("collision: trigger overlaps are reported, not resolved") {
    ecs::World world;
    registerSceneComponents(world);

    ecs::Entity zone = world.create();
    zone.set<Transform>({ Vec3 { 0, 0, 0 } });
    zone.set<Collider>({ Vec2 { 1, 1 }, true }); // trigger

    ecs::Entity mover = world.create();
    mover.set<Transform>({ Vec3 { 0.5f, 0, 0 } });
    mover.set<Collider>({ Vec2 { 1, 1 }, false });
    mover.set<Velocity>({});

    int overlaps = 0;
    forEachTriggerOverlap(world, [&](ecs::Entity dynamic, ecs::Entity trigger) {
        ++overlaps;
        CHECK(dynamic == mover);
        CHECK(trigger == zone);
    });
    CHECK(overlaps == 1);
    CHECK(mover.get<Transform>().position.x == 0.5f); // not pushed
}
