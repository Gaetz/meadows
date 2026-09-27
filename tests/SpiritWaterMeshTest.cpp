#include <doctest/doctest.h>

#include <cmath>

#include "world/spirit/SpiritWaterMesh.hpp"

using namespace world;

TEST_CASE("spirit water mesh: a jet tube follows its arc, ring to ring") {
    vector<Vec3> tris;
    const Vec3 origin { 0.0f, 1.6f, 0.0f };
    const Vec3 velocity { 10.0f, 5.0f, 0.0f };
    const u32 segments = 10;
    const u32 sides = 6;
    appendJetTube(tris, origin, velocity, 9.81f, 2.0f, 0.2f, segments, sides,
                  1.0f);
    // Two triangles per quad, sides x segments quads.
    CHECK(tris.size() == static_cast<size_t>(segments) * sides * 6);
    // The first ring hugs the origin, the last ring the arc's end.
    const Vec3 end { origin.x + velocity.x * 2.0f,
                     origin.y + velocity.y * 2.0f - 0.5f * 9.81f * 4.0f,
                     origin.z };
    f32 nearStart = 1e9f;
    f32 nearEnd = 1e9f;
    for (const Vec3& p : tris) {
        nearStart = glm::min(nearStart, glm::length(p - origin));
        nearEnd = glm::min(nearEnd, glm::length(p - end));
    }
    CHECK(nearStart == doctest::Approx(0.2f).epsilon(0.05));
    CHECK(nearEnd == doctest::Approx(0.2f).epsilon(0.05));
    // Nothing above the apex + radius, nothing below the end - radius.
    const f32 apex = origin.y + velocity.y * velocity.y / (2.0f * 9.81f);
    for (const Vec3& p : tris) {
        CHECK(p.y <= apex + 0.21f);
        CHECK(p.y >= end.y - 0.21f);
    }
    // Degenerate inputs add nothing.
    vector<Vec3> none;
    appendJetTube(none, origin, velocity, 9.81f, 0.0f, 0.2f);
    appendJetTube(none, origin, velocity, 9.81f, 1.0f, 0.2f, 0);
    CHECK(none.empty());
}

TEST_CASE("spirit water mesh: a blob is a closed sphere of the asked radius") {
    vector<Vec3> tris;
    const Vec3 center { 5.0f, 10.0f, -3.0f };
    const u32 rings = 7;
    const u32 sectors = 12;
    appendBlob(tris, center, 2.0f, rings, sectors, 1.0f);
    // Caps are single triangles: sectors x (2 x (rings - 1)) triangles.
    CHECK(tris.size() == static_cast<size_t>(sectors) * (rings - 1) * 2 * 3);
    for (const Vec3& p : tris) {
        CHECK(glm::length(p - center) == doctest::Approx(2.0f).epsilon(0.001));
    }
    // The squash flattens the vertical axis only.
    vector<Vec3> flat;
    appendBlob(flat, center, 2.0f, rings, sectors, 0.5f);
    f32 top = -1e9f;
    for (const Vec3& p : flat) {
        top = glm::max(top, p.y - center.y);
    }
    CHECK(top == doctest::Approx(1.0f));
}

TEST_CASE("spirit water mesh: radii follow flow and volume, clamped to what reads") {
    // 2 m³/s at 14 m/s: area 0.143 m² -> r ~ 0.21 m.
    CHECK(jetRadius(2.0f, 14.0f) == doctest::Approx(0.213f).epsilon(0.01));
    CHECK(jetRadius(0.0f, 14.0f) == 0.08f);   // floor: still visible
    CHECK(jetRadius(1000.0f, 1.0f) == 0.6f);  // ceiling
    // 60 m³ -> r = cbrt(3V / 4 pi) ~ 2.43 m.
    CHECK(blobRadius(60.0f) == doctest::Approx(2.43f).epsilon(0.01));
    CHECK(blobRadius(0.0f) == 0.25f);
    CHECK(blobRadius(1.0e6f) == 3.0f);
}
