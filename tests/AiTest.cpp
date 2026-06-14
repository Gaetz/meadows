#include <doctest/doctest.h>

#include "world/ai/Pathfinding.hpp"
#include "world/ai/Steering.hpp"

using namespace ai;

TEST_CASE("pathfinding: straight path on an open grid") {
    Grid grid { 5, 1 };
    const auto path = findPath(grid, { 0, 0 }, { 4, 0 });
    REQUIRE(path.size() == 5);
    CHECK(path.front() == GridCoord { 0, 0 });
    CHECK(path.back() == GridCoord { 4, 0 });
}

TEST_CASE("pathfinding: routes around a wall") {
    Grid grid { 3, 3 };
    grid.setBlocked(1, 0, true); // wall column at x=1 for y=0,1 leaves a gap at y=2
    grid.setBlocked(1, 1, true);

    const auto path = findPath(grid, { 0, 0 }, { 2, 0 });
    REQUIRE_FALSE(path.empty());
    CHECK(path.front() == GridCoord { 0, 0 });
    CHECK(path.back() == GridCoord { 2, 0 });
    CHECK(path.size() == 7); // must detour through y=2 (Manhattan 2 + 4 extra)
}

TEST_CASE("pathfinding: no path when the goal is walled off") {
    Grid grid { 3, 1 };
    grid.setBlocked(1, 0, true); // splits the 1-row grid
    CHECK(findPath(grid, { 0, 0 }, { 2, 0 }).empty());
}

TEST_CASE("pathfinding: blocked endpoints yield no path") {
    Grid grid { 3, 3 };
    grid.setBlocked(2, 2, true);
    CHECK(findPath(grid, { 0, 0 }, { 2, 2 }).empty());
}

TEST_CASE("ai: perception range and seek steering") {
    CHECK(withinRange({ 0, 0, 0 }, { 3, 4, 0 }, 5.0f));        // distance 5
    CHECK_FALSE(withinRange({ 0, 0, 0 }, { 3, 4, 0 }, 4.9f));

    const Vec3 velocity = seek({ 0, 0, 0 }, { 10, 0, 0 }, 2.0f);
    CHECK(velocity.x == doctest::Approx(2.0f)); // toward +x at speed 2
    CHECK(velocity.y == doctest::Approx(0.0f));

    const Vec3 still = seek({ 1, 1, 0 }, { 1, 1, 0 }, 2.0f);
    CHECK(still.x == 0.0f);
    CHECK(still.y == 0.0f);
}
