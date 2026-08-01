#include <doctest/doctest.h>

#include "engine/ecs/World.hpp"
#include "gameplay/actors/Swimming.hpp"
#include "world/scene/Components.hpp"
#include "world/scene/Floaters.hpp"

// Kinematic floating props (world/scene/Floaters) + the pure swim-drift
// helper: the two halves of the current's gameplay reach.

TEST_CASE("applyDrift pushes the target by the scaled current") {
    const Vec3 out = gameplay::applyDrift(Vec3 { 1.0f, -0.5f, 0.0f },
                                          Vec2 { 2.0f, -1.0f }, 0.5f);
    CHECK(out.x == doctest::Approx(2.0f));
    CHECK(out.y == doctest::Approx(-0.5f)); // vertical untouched
    CHECK(out.z == doctest::Approx(-0.5f));
}

TEST_CASE("floaters ride the surface and the current; dry props rest") {
    ecs::World world;
    world::registerSceneComponents(world);

    auto wet = world.handle().entity();
    wet.set(world::Transform { .position = { 10.0f, 3.0f, 0.0f } });
    wet.set(world::Floater { .driftFactor = 1.0f,
                             .draft = 0.1f,
                             .bobAmplitude = 0.0f });
    auto dry = world.handle().entity();
    dry.set(world::Transform { .position = { 500.0f, 7.0f, 0.0f } });
    dry.set(world::Floater {});

    const auto surface = [](const Vec3& at) -> std::optional<f32> {
        if (at.x < 100.0f) {
            return 5.0f; // a pond on the west side
        }
        return std::nullopt;
    };
    const auto flow = [](const Vec3&) { return Vec2 { 2.0f, 0.0f }; };

    world::updateFloaters(world, 0.5f, 0.0f, surface, flow);

    const auto& wetPos = wet.get<world::Transform>().position;
    CHECK(wetPos.x == doctest::Approx(11.0f)); // drifted 2 m/s * 0.5 s
    CHECK(wetPos.y == doctest::Approx(4.9f));  // surface - draft
    const auto& dryPos = dry.get<world::Transform>().position;
    CHECK(dryPos.x == doctest::Approx(500.0f)); // untouched
    CHECK(dryPos.y == doctest::Approx(7.0f));

    // Determinism: same start, same dt, same result.
    auto wet2 = world.handle().entity();
    wet2.set(world::Transform { .position = { 10.0f, 3.0f, 0.0f } });
    wet2.set(world::Floater { .driftFactor = 1.0f,
                              .draft = 0.1f,
                              .bobAmplitude = 0.0f });
    world::updateFloaters(world, 0.5f, 0.0f, surface, flow);
    const auto& pos2 = wet2.get<world::Transform>().position;
    CHECK(pos2.x == doctest::Approx(11.0f));
    CHECK(pos2.y == doctest::Approx(4.9f));
}
