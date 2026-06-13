#include <doctest/doctest.h>

#include "engine/ecs/World.hpp"

namespace {

struct Position {
    f32 x { 0.0f };
    f32 y { 0.0f };

    REFLECT_BEGIN(Position, void)
        REFLECT_FIELD(x)
        REFLECT_FIELD(y)
    REFLECT_END()
};

struct Velocity {
    f32 dx { 0.0f };
    f32 dy { 0.0f };

    REFLECT_BEGIN(Velocity, void)
        REFLECT_FIELD(dx)
        REFLECT_FIELD(dy)
    REFLECT_END()
};

} // namespace

TEST_CASE("ecs: create/set/get, destroy invalidates the handle") {
    ecs::World world;
    world.registerComponent<Position>();

    ecs::Entity e = world.create();
    CHECK(e.is_alive());

    e.set<Position>({ 1.0f, 2.0f });
    REQUIRE(e.try_get<Position>() != nullptr);
    CHECK(e.get<Position>().x == 1.0f);
    CHECK(e.get<Position>().y == 2.0f);

    e.destruct();
    CHECK_FALSE(e.is_alive());
}

TEST_CASE("ecs: add/has/remove") {
    ecs::World world;
    world.registerComponent<Position>();
    world.registerComponent<Velocity>();

    ecs::Entity e = world.create();
    e.set<Position>({});
    CHECK(e.has<Position>());
    CHECK_FALSE(e.has<Velocity>());

    e.set<Velocity>({});
    CHECK(e.has<Velocity>());

    e.remove<Velocity>();
    CHECK_FALSE(e.has<Velocity>());
}

TEST_CASE("ecs: a query iterates only entities with every component") {
    ecs::World world;
    world.registerComponent<Position>();
    world.registerComponent<Velocity>();

    for (i32 i = 0; i < 3; ++i) {
        world.create()
            .set<Position>({ static_cast<f32>(i), 0.0f })
            .set<Velocity>({ 1.0f, 0.0f });
    }
    world.create().set<Position>({ 9.0f, 9.0f }); // no Velocity → excluded

    i32 count = 0;
    world.handle().query<Position, Velocity>().each(
        [&](flecs::entity, Position& p, Velocity& v) {
            p.x += v.dx;
            ++count;
        });
    CHECK(count == 3);
}

TEST_CASE("ecs: InCell relation groups references and unloads them") {
    ecs::World world;
    world.registerComponent<Position>();

    ecs::Entity cell = world.create();
    for (i32 i = 0; i < 4; ++i) {
        world.create().set<Position>({}).add<ecs::InCell>(cell);
    }

    const auto countInCell = [&] {
        i32 n = 0;
        world.handle().query_builder().with<ecs::InCell>(cell).build().each(
            [&](flecs::entity) { ++n; });
        return n;
    };
    CHECK(countInCell() == 4);

    world.handle().delete_with<ecs::InCell>(cell); // cell unload
    CHECK(countInCell() == 0);
}

TEST_CASE("ecs: registerComponent bridges flecs ids to reflection") {
    ecs::World world;
    const ecs::Entity component = world.registerComponent<Position>();

    CHECK(world.reflectedComponent(component.id()) == &Position::staticTypeInfo());
    CHECK(world.reflectedComponent(0) == nullptr);
}
