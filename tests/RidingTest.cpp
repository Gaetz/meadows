#include <doctest/doctest.h>

#include "gameplay/actors/Riding.hpp"

using gameplay::RideState;
using gameplay::stepRide;

TEST_CASE("riding: converges to mount speed on flat ground (É11)") {
    const auto flat = [](f32, f32) { return 0.0f; };
    RideState state;
    // One second of full-forward wish at 60 Hz, generous smoothing.
    for (int i = 0; i < 60; ++i) {
        state = stepRide(state, { 0.0f, 0.0f, 1.0f }, 9.0f, 10.0f,
                         1.0f / 60.0f, flat);
    }
    CHECK(state.velocity.z == doctest::Approx(9.0f).epsilon(0.01));
    CHECK(state.velocity.x == doctest::Approx(0.0f));
    // Distance: ramp-up eats a little of the first second.
    CHECK(state.position.z > 8.0f);
    CHECK(state.position.z < 9.0f);
    CHECK(state.position.y == doctest::Approx(0.0f));
}

TEST_CASE("riding: zero wish coasts to a stop, y stays grounded (É11)") {
    const auto flat = [](f32, f32) { return 2.5f; };
    RideState state;
    state.velocity = { 9.0f, 0.0f, 0.0f };
    for (int i = 0; i < 120; ++i) {
        state = stepRide(state, { 0.0f, 0.0f, 0.0f }, 9.0f, 10.0f,
                         1.0f / 60.0f, flat);
    }
    CHECK(glm::length(state.velocity) < 0.01f);
    // The ground hug pulled y onto the 2.5 m plateau.
    CHECK(state.position.y == doctest::Approx(2.5f).epsilon(0.01));
}

TEST_CASE("riding: the height follows a slope without popping (É11)") {
    // A 20% slope along +X.
    const auto slope = [](f32 x, f32) { return 0.2f * x; };
    RideState state;
    f32 maxJump = 0.0f;
    f32 lastY = 0.0f;
    for (int i = 0; i < 180; ++i) {
        state = stepRide(state, { 1.0f, 0.0f, 0.0f }, 9.0f, 10.0f,
                         1.0f / 60.0f, slope);
        maxJump = glm::max(maxJump, glm::abs(state.position.y - lastY));
        lastY = state.position.y;
    }
    // Tracks the slope (within the smoothing lag)...
    CHECK(state.position.y ==
          doctest::Approx(0.2f * state.position.x).epsilon(0.15));
    // ...and never teleports vertically (9 m/s on 20% climbs ~1.8 m/s:
    // ~0.03 m per frame plus the smoothing catch-up).
    CHECK(maxJump < 0.08f);

    // The wish's y component is ignored — the ground owns the height.
    RideState pitched;
    const auto flat = [](f32, f32) { return 0.0f; };
    pitched = stepRide(pitched, { 0.0f, 5.0f, 1.0f }, 9.0f, 10.0f, 0.1f,
                       flat);
    CHECK(pitched.velocity.y == 0.0f);
}
