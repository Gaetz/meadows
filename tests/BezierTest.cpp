#include <doctest/doctest.h>

#include <cmath>

#include "engine/core/Bezier.hpp"

// The cubic Bézier / polyline toolkit: rivers smooth their course with
// it, roads/splines will follow.

TEST_CASE("bezier evaluation hits endpoints and bows toward controls") {
    const Vec3 p0 { 0.0f, 0.0f, 0.0f };
    const Vec3 p1 { 0.0f, 0.0f, 1.0f };
    const Vec3 p2 { 1.0f, 0.0f, 1.0f };
    const Vec3 p3 { 1.0f, 0.0f, 0.0f };
    CHECK(glm::distance(core::bezierPoint(p0, p1, p2, p3, 0.0f), p0) <
          1e-6f);
    CHECK(glm::distance(core::bezierPoint(p0, p1, p2, p3, 1.0f), p3) <
          1e-6f);
    const Vec3 mid = core::bezierPoint(p0, p1, p2, p3, 0.5f);
    CHECK(mid.x == doctest::Approx(0.5f));
    CHECK(mid.z == doctest::Approx(0.75f)); // pulled toward the controls
    // Tangent at the start points along p1 - p0.
    const Vec3 tan = core::bezierTangent(p0, p1, p2, p3, 0.0f);
    CHECK(tan.z > 0.0f);
    CHECK(std::abs(tan.x) < 1e-5f);
}

TEST_CASE("polyline simplification drops grid stair-steps") {
    // A staircase along +X/+Z at 8 m steps: RDP at ~7 m keeps only the
    // corners of the overall diagonal.
    vector<Vec3> stairs;
    f32 x = 0.0f, z = 0.0f;
    for (u32 i = 0; i < 20; ++i) {
        stairs.push_back({ x, 0.0f, z });
        if (i % 2 == 0) {
            x += 8.0f;
        } else {
            z += 8.0f;
        }
    }
    const auto simple = core::simplifyPolylineXz(stairs, 7.0f);
    CHECK(simple.size() < stairs.size() / 2);
    CHECK(glm::distance(simple.front(), stairs.front()) < 1e-6f);
    CHECK(glm::distance(simple.back(), stairs.back()) < 1e-6f);
}

TEST_CASE("polyline smoothing rounds corners and keeps endpoints") {
    // A hard 90° corner: the smoothed course must turn progressively.
    const vector<Vec3> corner = { { 0.0f, 0.0f, 0.0f },
                                  { 40.0f, 0.0f, 0.0f },
                                  { 40.0f, 0.0f, 40.0f } };
    const auto smooth = core::smoothPolyline(corner, 4.0f);
    REQUIRE(smooth.size() > 10);
    CHECK(glm::distance(smooth.front(), corner.front()) < 1e-5f);
    CHECK(glm::distance(smooth.back(), corner.back()) < 1e-5f);
    // Max turn between consecutive segments stays well under 90°.
    f32 worstTurn = 0.0f;
    for (size_t i = 2; i < smooth.size(); ++i) {
        const Vec3 a = glm::normalize(smooth[i - 1] - smooth[i - 2]);
        const Vec3 b = glm::normalize(smooth[i] - smooth[i - 1]);
        worstTurn = std::max(worstTurn,
                             std::acos(glm::clamp(glm::dot(a, b), -1.0f,
                                                  1.0f)));
    }
    CHECK(worstTurn < 0.8f); // radians; the raw corner is pi/2
    // Interpolating spline: the corner point itself is on the curve.
    f32 nearest = 1e9f;
    for (const Vec3& p : smooth) {
        nearest = std::min(nearest, glm::distance(p, corner[1]));
    }
    CHECK(nearest < 0.5f);
}
