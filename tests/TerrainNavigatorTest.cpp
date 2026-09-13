#include <doctest/doctest.h>

#include "world/ai/TerrainNavigator.hpp"

// The nav seam's sanctioned fallback: lazy 3D A* over a height
// function, honoring slopes and blocking boxes.

TEST_CASE("terrain navigator goes around a blocking box") {
    world::TerrainNavigator nav { [](f32, f32) { return 0.0f; } };
    // A wall across the straight line from (0,0) to (20,0).
    nav.setBlockingBoxes({ { { 8.0f, -1.0f, -6.0f },
                             { 12.0f, 3.0f, 6.0f } } });
    const nav::PathResult path =
        nav.findPath({ { 0.0f, 0.0f, 0.0f }, { 20.0f, 0.0f, 0.0f }, 1.0f });
    REQUIRE(path.success);
    REQUIRE(path.waypoints.size() >= 3);
    // Some waypoint must detour beyond the wall's half-depth.
    bool detoured = false;
    for (const Vec3& p : path.waypoints) {
        if (std::abs(p.z) > 5.5f) {
            detoured = true;
        }
        // And none may stand inside the box.
        const bool inside =
            p.x > 8.0f && p.x < 12.0f && std::abs(p.z) < 6.0f;
        CHECK_FALSE(inside);
    }
    CHECK(detoured);
}

TEST_CASE("blocking-box index: straddling and negative cells still block") {
    // Boxes crossing the 16 m index-cell boundaries (positive AND
    // negative coordinates) must be found from any point inside them —
    // the cell registration covers every touched cell.
    world::TerrainNavigator nav { [](f32, f32) { return 0.0f; } };
    nav.setBlockingBoxes({
        // Straddles x = 16 and z = 0 lattice lines.
        { { 12.0f, -1.0f, -4.0f }, { 20.0f, 3.0f, 4.0f } },
        // Fully in negative-coordinate cells.
        { { -40.0f, -1.0f, -40.0f }, { -24.0f, 3.0f, -24.0f } },
    });
    // A path THROUGH the straddling box must detour around it.
    const nav::PathResult path =
        nav.findPath({ { 0.0f, 0.0f, 0.0f }, { 32.0f, 0.0f, 0.0f }, 1.0f });
    REQUIRE(path.success);
    for (const Vec3& p : path.waypoints) {
        const bool inside = p.x > 12.0f && p.x < 20.0f &&
                            std::abs(p.z) < 4.0f;
        CHECK_FALSE(inside);
    }
    // Same through the negative-cell box.
    const nav::PathResult neg = nav.findPath(
        { { -48.0f, 0.0f, -32.0f }, { -16.0f, 0.0f, -32.0f }, 1.0f });
    REQUIRE(neg.success);
    for (const Vec3& p : neg.waypoints) {
        const bool inside = p.x > -40.0f && p.x < -24.0f &&
                            p.z > -40.0f && p.z < -24.0f;
        CHECK_FALSE(inside);
    }
}

TEST_CASE("terrain navigator refuses cliffs and rides the height") {
    // A 5 m cliff at x = 10 splits the world; a ramp exists at z >= 20.
    const auto height = [](f32 x, f32 z) {
        if (x < 10.0f) {
            return 0.0f;
        }
        if (z >= 20.0f) { // the ramp region: gentle rise
            return glm::min((x - 10.0f) * 0.5f, 5.0f);
        }
        return 5.0f; // sheer cliff
    };
    world::TerrainNavigator nav { height };
    const nav::PathResult path =
        nav.findPath({ { 0.0f, 0.0f, 25.0f }, { 30.0f, 5.0f, 25.0f }, 1.5f });
    REQUIRE(path.success);
    // Waypoints carry the terrain height (the mover walks the ramp).
    for (const Vec3& p : path.waypoints) {
        CHECK(p.y <= 5.01f);
    }
    // Direct line at z = 0 would hit the cliff: no path within a small
    // budget when the ramp is out of reach.
    world::TerrainNavigator strict { height };
    strict.maxExpansions = 300; // too small to discover the far ramp
    const nav::PathResult none = strict.findPath(
        { { 0.0f, 0.0f, 0.0f }, { 30.0f, 5.0f, 0.0f }, 1.0f });
    CHECK_FALSE(none.success);
}
