#include <doctest/doctest.h>

#include <glm/glm.hpp>

#include "world/ai/Perception.hpp"

// The perception state machine, sim-pure: the vision
// cone, sight memory (Alert -> Searching -> Calm), and hearing.

using world::AwareState;
using world::Perception;

TEST_CASE("the vision cone sees ahead within range, not behind or past "
          "it") {
    Perception p; // 14 m, 140 degrees
    const Vec3 self { 0.0f, 0.0f, 0.0f };
    const Vec3 facing { 0.0f, 0.0f, 1.0f };

    CHECK(world::inViewCone(p, self, facing, { 0.0f, 0.0f, 8.0f }));
    CHECK(world::inViewCone(p, self, facing, { 6.0f, 0.0f, 4.0f }));
    // Behind: never.
    CHECK(!world::inViewCone(p, self, facing, { 0.0f, 0.0f, -3.0f }));
    // 80 degrees off-axis > the 70-degree half angle.
    CHECK(!world::inViewCone(
        p, self, facing,
        { 8.0f * std::sin(glm::radians(80.0f)), 0.0f,
          8.0f * std::cos(glm::radians(80.0f)) }));
    // Past the view distance.
    CHECK(!world::inViewCone(p, self, facing, { 0.0f, 0.0f, 20.0f }));
    // On top of the perceiver: seen regardless of facing.
    CHECK(world::inViewCone(p, self, facing, self));
}

TEST_CASE("sight alerts; losing it degrades Alert -> Searching -> Calm on "
          "the timers") {
    Perception p;
    p.memorySeconds = 2.0f;
    p.searchSeconds = 3.0f;
    const Vec3 seen { 5.0f, 0.0f, 5.0f };

    world::updatePerception(p, true, seen, 0.1f);
    CHECK(world::awareState(p) == AwareState::Alert);
    CHECK(p.lastKnownPos.x == doctest::Approx(5.0f));

    // Lost, but within memory: still Alert (hunting the last position).
    world::updatePerception(p, false, {}, 1.5f);
    CHECK(world::awareState(p) == AwareState::Alert);
    // Memory expires: an explicit search of the last known spot.
    world::updatePerception(p, false, {}, 1.0f);
    CHECK(world::awareState(p) == AwareState::Searching);
    CHECK(p.lastKnownPos.x == doctest::Approx(5.0f)); // spot remembered
    // The search patience runs out: back to Calm.
    world::updatePerception(p, false, {}, 3.5f);
    CHECK(world::awareState(p) == AwareState::Calm);

    // Re-acquiring sight from ANY state snaps back to Alert.
    world::updatePerception(p, true, seen, 0.1f);
    CHECK(world::awareState(p) == AwareState::Alert);
}

TEST_CASE("noise makes a calm perceiver suspicious — within earshot only") {
    Perception p; // hearing 12 m
    const Vec3 self { 0.0f, 0.0f, 0.0f };

    // Too far: ignored.
    world::hearNoise(p, self, { 30.0f, 0.0f, 0.0f });
    CHECK(world::awareState(p) == AwareState::Calm);

    // In range: investigate the position.
    world::hearNoise(p, self, { 6.0f, 0.0f, 2.0f });
    CHECK(world::awareState(p) == AwareState::Suspicious);
    CHECK(p.lastKnownPos.x == doctest::Approx(6.0f));

    // Suspicion decays to Calm without confirmation.
    world::updatePerception(p, false, {}, p.searchSeconds + 0.1f);
    CHECK(world::awareState(p) == AwareState::Calm);

    // A sneaked noise (loudness 0.5) carries HALF as far: 8 m is
    // outside the effective 6 m, 5 m is inside.
    Perception q;
    world::hearNoise(q, self, { 8.0f, 0.0f, 0.0f }, 0.5f);
    CHECK(world::awareState(q) == AwareState::Calm);
    world::hearNoise(q, self, { 5.0f, 0.0f, 0.0f }, 0.5f);
    CHECK(world::awareState(q) == AwareState::Suspicious);
}

TEST_CASE("noise re-aims a search and restarts its patience; Alert "
          "ignores it") {
    Perception p;
    p.memorySeconds = 1.0f;
    p.searchSeconds = 4.0f;
    const Vec3 self { 0.0f, 0.0f, 0.0f };

    // Alert on sight; a noise elsewhere must NOT move the known position.
    world::updatePerception(p, true, { 3.0f, 0.0f, 0.0f }, 0.1f);
    world::hearNoise(p, self, { -5.0f, 0.0f, 0.0f });
    CHECK(world::awareState(p) == AwareState::Alert);
    CHECK(p.lastKnownPos.x == doctest::Approx(3.0f));

    // Degrade to Searching, burn most of the patience...
    world::updatePerception(p, false, {}, 1.5f);
    CHECK(world::awareState(p) == AwareState::Searching);
    world::updatePerception(p, false, {}, 3.0f);
    CHECK(world::awareState(p) == AwareState::Searching);
    // ...then a fresh noise re-aims it AND restarts the clock: another
    // 3 s later it is still searching (a stale clock would be Calm).
    world::hearNoise(p, self, { 0.0f, 0.0f, 7.0f });
    CHECK(p.lastKnownPos.z == doctest::Approx(7.0f));
    world::updatePerception(p, false, {}, 3.0f);
    CHECK(world::awareState(p) == AwareState::Searching);
    world::updatePerception(p, false, {}, 1.5f);
    CHECK(world::awareState(p) == AwareState::Calm);
}
