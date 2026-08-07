#include <doctest/doctest.h>

#include <filesystem>

#include "engine/dungeon/DungeonBake.hpp"
#include "world/ai/InteriorNavigator.hpp"

using namespace dungeon;

namespace {

// Hand-built worlds: boxes of air in infinite rock.
DensityFn boxAir(const Vec3& lo, const Vec3& hi) {
    return [lo, hi](const Vec3& p) {
        const Vec3 d = glm::max(lo - p, p - hi);
        return glm::max(d.x, glm::max(d.y, d.z));
    };
}

DensityFn unionAir(DensityFn a, DensityFn b) {
    return [a = std::move(a), b = std::move(b)](const Vec3& p) {
        return glm::min(a(p), b(p));
    };
}

} // namespace

TEST_CASE("dungeon nav: two floors over the same column both bake as levels") {
    // A gallery at y [0, 3] and another at y [8, 11], same footprint.
    const DensityFn density =
        unionAir(boxAir({ 0, 0, 0 }, { 10, 3, 4 }),
                 boxAir({ 0, 8, 0 }, { 10, 11, 4 }));
    const NavGrid grid = bakeNavGrid(density, { -1, -2, -1 }, { 11, 13, 5 },
                                     0.5f, 1.8f);
    REQUIRE_FALSE(grid.empty());
    const u32 column = grid.columnOf(5.0f, 2.0f);
    REQUIRE(column != ~0u);
    u32 begin = 0;
    u32 end = 0;
    grid.columnLevels(column, begin, end);
    REQUIRE(end - begin == 2);
    CHECK(grid.levels[begin].floorY < 1.0f);
    CHECK(grid.levels[begin + 1].floorY > 7.0f);
    CHECK(grid.levels[begin].clearance >= 1.8f);
}

TEST_CASE("dungeon nav: no path between stacked floors without a ramp") {
    const DensityFn density =
        unionAir(boxAir({ 0, 0, 0 }, { 20, 3, 4 }),
                 boxAir({ 0, 8, 0 }, { 20, 11, 4 }));
    const NavGrid grid = bakeNavGrid(density, { -1, -2, -1 }, { 21, 13, 5 },
                                     0.5f, 1.8f);
    const world::InteriorNavigator navigator { grid };
    const nav::PathResult flat = navigator.findPath(
        { { 1.0f, 0.2f, 2.0f }, { 19.0f, 0.2f, 2.0f }, 0.5f });
    CHECK(flat.success);
    const nav::PathResult up = navigator.findPath(
        { { 1.0f, 0.2f, 2.0f }, { 19.0f, 8.2f, 2.0f }, 0.5f });
    CHECK_FALSE(up.success);
}

TEST_CASE("dungeon nav: a carved ramp connects the floors") {
    // Flat low gallery, flat high gallery, and a sloped box between them.
    DensityFn density =
        unionAir(boxAir({ 0, 0, 0 }, { 10, 3, 4 }),
                 boxAir({ 30, 8, 0 }, { 40, 11, 4 }));
    // The ramp: air where y is within [slope(x), slope(x) + 3] over x in
    // [10, 30]; slope rises 0 -> 8.
    density = unionAir(std::move(density), [](const Vec3& p) {
        if (p.x < 9.5f || p.x > 30.5f || p.z < 0.0f || p.z > 4.0f) {
            return 1.0f;
        }
        const f32 t = glm::clamp((p.x - 10.0f) / 20.0f, 0.0f, 1.0f);
        const f32 floor = t * 8.0f;
        const f32 lo = floor - p.y;         // below the ramp floor: rock
        const f32 hi = p.y - (floor + 3.0f); // above the headroom: rock
        return glm::max(lo, hi);
    });
    const NavGrid grid = bakeNavGrid(density, { -1, -2, -1 }, { 41, 13, 5 },
                                     0.5f, 1.8f);
    const world::InteriorNavigator navigator { grid };
    const nav::PathResult path = navigator.findPath(
        { { 2.0f, 0.2f, 2.0f }, { 38.0f, 8.2f, 2.0f }, 0.5f });
    REQUIRE(path.success);
    REQUIRE(path.waypoints.size() > 2);
    // The path actually climbs: floors along the way rise monotonically-ish.
    CHECK(path.waypoints.front().y < 1.0f);
    CHECK(path.waypoints.back().y > 7.0f);
}

TEST_CASE("dungeon nav: nvg files round-trip and refuse stale versions") {
    const DensityFn density = boxAir({ 0, 0, 0 }, { 6, 3, 6 });
    const NavGrid grid = bakeNavGrid(density, { -1, -2, -1 }, { 7, 5, 7 },
                                     0.5f, 1.8f);
    REQUIRE_FALSE(grid.empty());
    const auto path =
        std::filesystem::temp_directory_path() / "meadows_navgrid.nvg";
    REQUIRE(writeNvgFile(path, grid, kDungeonBakeVersion));
    CHECK_FALSE(readNvgFile(path, kDungeonBakeVersion + 1).has_value());
    const auto loaded = readNvgFile(path, kDungeonBakeVersion);
    REQUIRE(loaded.has_value());
    CHECK(loaded->width == grid.width);
    CHECK(loaded->depth == grid.depth);
    REQUIRE(loaded->levels.size() == grid.levels.size());
    CHECK(loaded->firstLevel == grid.firstLevel);
    for (size_t i = 0; i < grid.levels.size(); ++i) {
        CHECK(loaded->levels[i].floorY == grid.levels[i].floorY);
        CHECK(loaded->levels[i].clearance == grid.levels[i].clearance);
    }
    std::filesystem::remove(path);
}

TEST_CASE("dungeon nav: a baked mine is walkable from entrance to goal") {
    DungeonParams params;
    params.seed = 42;
    params.space.gridX = 6;
    params.space.gridZ = 6;
    params.space.floors = 2;
    params.voxelSize = 1.2f;
    const DungeonBakeResult r = bakeDungeon(params);
    REQUIRE_FALSE(r.empty());
    REQUIRE_FALSE(r.navGrid.empty());

    const world::InteriorNavigator navigator { r.navGrid };
    const Vec3 goal = roomCenter(r.space, r.space.goal) +
                      Vec3 { 0.0f, 0.3f, 0.0f };
    const nav::PathResult path =
        navigator.findPath({ r.entrancePos, goal, 1.5f });
    CHECK(path.success);
}
