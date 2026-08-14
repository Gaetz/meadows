#include <doctest/doctest.h>

#include <cmath>
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

// The playtest-found ramp-bottom step: a flat corridor's end cap burrows a
// pocket under the rising ramp floor, ending in a knee-high rock face. The
// carve cuts all air below a ramp's floor strip (DensityField::cutsBelow);
// this walks the junction and asserts no riser above the character step.
TEST_CASE("dungeon nav: a ramp joins its corridor without a step") {
    SpaceGraph graph;
    graph.params = SpaceParams {};
    SpaceEdge edge;
    // Flat hop into the ramp's bottom cell, then the ramp itself.
    edge.path = { { 1, 2, 1 }, { 2, 2, 1 }, { 3, 2, 0 } };
    graph.edges.push_back(edge);
    DensityParams density { 1337 };
    density.corridorWobble = 0.0f; // scan a straight line down the tube
    const DensityField field(graph, density);

    const Vec3 a = slotCenter(graph.params, edge.path[0]);
    const Vec3 dir { 1.0f, 0.0f, 0.0f };
    const auto floorAtS = [&](f32 s) {
        const Vec3 base = a + dir * s;
        for (f32 y = 14.5f; y > -3.0f; y -= 0.02f) {
            if (field.sample({ base.x, a.y + y, base.z }) < 0.0f) {
                f32 yy = y;
                while (yy > -3.0f &&
                       field.sample(
                           { base.x, a.y + yy - 0.02f, base.z }) < 0.0f) {
                    yy -= 0.02f;
                }
                return a.y + yy;
            }
        }
        return -999.0f;
    };
    f32 prev = floorAtS(0.0f);
    f32 worstRise = 0.0f;
    for (f32 s = 0.05f; s < 28.0f; s += 0.05f) {
        const f32 f = floorAtS(s);
        if (f > -900.0f && prev > -900.0f) {
            worstRise = glm::max(worstRise, f - prev);
        }
        if (f - prev > 0.35f) {
            MESSAGE("rise " << f - prev << " at s=" << s << " (floor "
                            << prev << " -> " << f << ")");
        }
        prev = f;
    }
    // Jolt's stair step-up is 0.4; wall noise may leave small bumps.
    CHECK(worstRise < 0.35f);
}

TEST_CASE("dungeon nav: every gameplay anchor stands on a walkable floor") {
    // The tool's default mine (seed 1337, grid 8, 2 floors): an anchor the
    // nav grid cannot snap is a prop buried in rock or floating mid-slope —
    // invisible or unreachable in game.
    DungeonParams params;
    params.seed = 1337;
    params.voxelSize = 1.8f;
    const DungeonBakeResult r = bakeDungeon(params);
    REQUIRE_FALSE(r.empty());
    REQUIRE_FALSE(r.navGrid.empty());

    const auto walkable = [&](const Vec3& p) {
        const u32 column = r.navGrid.columnOf(p.x, p.z);
        if (column == ~0u) {
            return false;
        }
        u32 begin = 0;
        u32 end = 0;
        r.navGrid.columnLevels(column, begin, end);
        for (u32 l = begin; l < end; ++l) {
            if (std::abs(r.navGrid.levels[l].floorY - p.y) <= 0.6f) {
                return true;
            }
        }
        return false;
    };
    const auto checkFamily = [&](const char* tag,
                                 const vector<DungeonBakeResult::Anchor>&
                                     family) {
        for (size_t i = 0; i < family.size(); ++i) {
            INFO(tag << " " << i << " at (" << family[i].position.x << ", "
                     << family[i].position.y << ", " << family[i].position.z
                     << ")");
            CHECK(walkable(family[i].position));
        }
    };
    // Actors demand more than a walkable column: a wall-clear one (the
    // agent-radius story — a spawn hugging the rock stands half inside it).
    const auto standable = [&](const Vec3& p) {
        const u32 column = r.navGrid.columnOf(p.x, p.z);
        if (column == ~0u) {
            return false;
        }
        const i32 ix = static_cast<i32>(column % r.navGrid.width);
        const i32 iz = static_cast<i32>(column / r.navGrid.width);
        return !r.navGrid.wallAdjacent(ix, iz, p.y);
    };
    REQUIRE_FALSE(r.enemySpawns.empty());
    for (size_t i = 0; i < r.enemySpawns.size(); ++i) {
        INFO("enemy " << i);
        CHECK(standable(r.enemySpawns[i].position));
    }
    for (size_t i = 0; i < r.npcSpawns.size(); ++i) {
        INFO("npc " << i);
        CHECK(standable(r.npcSpawns[i].position));
    }
    checkFamily("enemy", r.enemySpawns);
    checkFamily("lever", r.levers);
    checkFamily("chest", r.chests);
    checkFamily("vein", r.oreVeins);
    checkFamily("npc", r.npcSpawns);
    checkFamily("patrol", r.patrolPoints);
    checkFamily("barrier", r.barriers);
}

TEST_CASE("dungeon nav: a baked mine is walkable from entrance to goal") {
    DungeonParams params;
    params.seed = 42;
    params.space.gridX = 6;
    params.space.gridZ = 6;
    params.space.floors = 2;
    params.voxelSize = 1.4f;
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
