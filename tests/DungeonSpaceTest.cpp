#include <doctest/doctest.h>

#include <cmath>

#include "engine/dungeon/SpaceGraph.hpp"

using namespace dungeon;

namespace {

SpaceParams spaceParams(u32 seed) {
    SpaceParams p;
    p.seed = seed;
    p.gridX = 7;
    p.gridZ = 7;
    p.floors = 3;
    return p;
}

MissionParams missionParams(u32 seed) {
    MissionParams p;
    p.seed = seed;
    p.subCycles = 2;
    p.maxDepth = 2;
    return p;
}

} // namespace

TEST_CASE("dungeon space: embedding is deterministic and solvable over seeds") {
    for (u32 seed = 1; seed <= 60; ++seed) {
        const MissionGraph mission = buildMissionGraph(missionParams(seed));
        const SpaceGraph a = buildSpaceGraph(mission, spaceParams(seed));
        const SpaceGraph b = buildSpaceGraph(mission, spaceParams(seed));
        CAPTURE(seed);
        REQUIRE_FALSE(a.rooms.empty());
        CHECK(toAscii(a) == toAscii(b));
        CHECK(a.rooms.size() == mission.nodes.size());
        CHECK(a.edges.size() == mission.edges.size());
        CHECK(isSolvable(mission, a));
    }
}

TEST_CASE("dungeon space: rooms occupy distinct slots, tall rooms span floors") {
    const MissionGraph mission = buildMissionGraph(missionParams(9));
    SpaceParams params = spaceParams(9);
    params.tallRoomChance = 1.0; // force tall rooms wherever the slot above is free
    const SpaceGraph g = buildSpaceGraph(mission, params);
    REQUIRE_FALSE(g.rooms.empty());

    // No two rooms may share a slot (tall rooms occupy two).
    vector<GridPos> taken;
    bool anyTall = false;
    for (const SpaceRoom& room : g.rooms) {
        for (i32 s = 0; s < room.floorSpan; ++s) {
            const GridPos slot { room.pos.x, room.pos.z, room.pos.floor + s };
            for (const GridPos& t : taken) {
                CHECK_FALSE(t == slot);
            }
            taken.push_back(slot);
        }
        anyTall = anyTall || room.floorSpan > 1;
        CHECK(room.pos.floor + room.floorSpan <= params.floors);
    }
    CHECK(anyTall);
}

TEST_CASE("dungeon space: corridors only descend vertically on one-way edges") {
    for (u32 seed = 1; seed <= 40; ++seed) {
        MissionParams mp = missionParams(seed);
        mp.patterns = { CyclePattern::BlockedRetreat };
        const MissionGraph mission = buildMissionGraph(mp);
        const SpaceGraph g = buildSpaceGraph(mission, spaceParams(seed));
        CAPTURE(seed);
        REQUIRE_FALSE(g.rooms.empty());
        for (const SpaceEdge& e : g.edges) {
            for (size_t i = 1; i < e.path.size(); ++i) {
                const GridPos& a = e.path[i - 1];
                const GridPos& b = e.path[i];
                const bool vertical = a.x == b.x && a.z == b.z &&
                                      a.floor != b.floor;
                if (vertical) {
                    CHECK(e.oneWay);          // drops only on one-way edges
                    CHECK(b.floor == a.floor + 1); // and only downward
                }
            }
        }
    }
}

TEST_CASE("dungeon space: the goal anchors far from the entrance and deep") {
    for (u32 seed = 1; seed <= 30; ++seed) {
        const MissionGraph mission = buildMissionGraph(missionParams(seed));
        const SpaceGraph g = buildSpaceGraph(mission, spaceParams(seed));
        CAPTURE(seed);
        REQUIRE_FALSE(g.rooms.empty());
        const SpaceRoom& entrance = g.rooms[g.entrance];
        const SpaceRoom& goal = g.rooms[g.goal];
        // Far: at least half the (possibly grown) grid away in x.
        CHECK(goal.pos.x - entrance.pos.x >= g.params.gridX / 2 - 3);
        // Deep: the bottom half of the floors.
        CHECK(goal.pos.floor >= g.params.floors / 2);
    }
}

TEST_CASE("dungeon space: entrance sits near the grid edge, floor 0") {
    const MissionGraph mission = buildMissionGraph(missionParams(21));
    const SpaceGraph g = buildSpaceGraph(mission, spaceParams(21));
    REQUIRE_FALSE(g.rooms.empty());
    const SpaceRoom& entrance = g.rooms[g.entrance];
    CHECK(entrance.pos.x == 2);
    CHECK(entrance.pos.floor == 0);
}

TEST_CASE("dungeon space: world positions stack floors downward in Y") {
    SpaceParams p = spaceParams(1);
    const Vec3 top = slotCenter(p, { 2, 3, 0 });
    const Vec3 below = slotCenter(p, { 2, 3, 1 });
    // The per-slot height jitter is shared by every floor of a column, so
    // the floor spacing stays exact and the jitter stays bounded.
    CHECK(std::abs(top.y) <= p.slotHeightJitter);
    CHECK(below.y - top.y == -p.floorSpacing);
    CHECK(top.x == below.x);
    CHECK(top.z == below.z);
}

TEST_CASE("dungeon space: an impossible fit returns an empty graph, no crash") {
    const MissionGraph mission = buildMissionGraph(missionParams(5));
    SpaceParams p = spaceParams(5);
    p.gridX = 2;
    p.gridZ = 1;
    p.floors = 1;  // fewer slots than mission nodes
    p.attempts = 4; // below the growth threshold: the grid stays tiny
    const SpaceGraph g = buildSpaceGraph(mission, p);
    CHECK(g.rooms.empty());
}
