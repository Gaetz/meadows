#include <doctest/doctest.h>

#include "engine/platform/Input.hpp"

// The pure half of the gamepad channel: the radial
// stick deadzone. The SDL-backed polling is smoke-tested in game; the
// MATH is what must not drift.

using platform::applyDeadzone;

TEST_CASE("deadzone: a resting or drifting stick reads exactly zero") {
    CHECK(applyDeadzone({ 0.0f, 0.0f }, 0.15f).x == 0.0f);
    const Vec2 drift = applyDeadzone({ 0.1f, -0.05f }, 0.15f);
    CHECK(drift.x == 0.0f);
    CHECK(drift.y == 0.0f);
    // Exactly on the edge is still dead (<=).
    CHECK(applyDeadzone({ 0.15f, 0.0f }, 0.15f).x == 0.0f);
}

TEST_CASE("deadzone: the live band rescales to 0..1 with no step") {
    // Just past the zone: barely alive, not a jump to 0.15.
    const Vec2 justPast = applyDeadzone({ 0.16f, 0.0f }, 0.15f);
    CHECK(justPast.x > 0.0f);
    CHECK(justPast.x < 0.02f);
    // Halfway through the band lands halfway through the output.
    const Vec2 mid = applyDeadzone({ 0.575f, 0.0f }, 0.15f);
    CHECK(mid.x == doctest::Approx(0.5f));
    // Full deflection stays fully reachable.
    CHECK(applyDeadzone({ 1.0f, 0.0f }, 0.15f).x == doctest::Approx(1.0f));
}

TEST_CASE("deadzone: radial — direction is preserved, corners clamped") {
    const Vec2 diagonal = applyDeadzone({ 0.6f, 0.6f }, 0.15f);
    CHECK(diagonal.x == doctest::Approx(diagonal.y)); // direction kept
    // A square-gate corner (len > 1) clamps to unit magnitude.
    const Vec2 corner = applyDeadzone({ 1.0f, 1.0f }, 0.15f);
    const f32 len =
        std::sqrt(corner.x * corner.x + corner.y * corner.y);
    CHECK(len == doctest::Approx(1.0f));
}

TEST_CASE("deadzone: a degenerate zone (>= 1) kills everything") {
    CHECK(applyDeadzone({ 0.9f, 0.0f }, 1.0f).x == 0.0f);
    CHECK(applyDeadzone({ 1.0f, 1.0f }, 1.5f).y == 0.0f);
}
