#include <doctest/doctest.h>

#include <cmath>

#include "engine/dungeon/DungeonBake.hpp"

using namespace dungeon;

namespace {

DungeonParams mineParams(u32 seed) {
    DungeonParams p;
    p.seed = seed;
    p.space.gridX = 6;
    p.space.gridZ = 6;
    p.space.floors = 2;
    p.voxelSize = 1.8f; // coarse: keeps the test fast, the contract identical
    return p;
}

} // namespace

TEST_CASE("dungeon bake: produces cell meshes, torches and an entrance") {
    const DungeonBakeResult r = bakeDungeon(mineParams(42));
    REQUIRE_FALSE(r.empty());
    CHECK_FALSE(r.torches.empty());
    CHECK(r.space.rooms.size() == r.mission.nodes.size());

    for (const auto& cellMesh : r.cellMeshes) {
        CHECK_FALSE(cellMesh.mesh.vertices.empty());
        CHECK(cellMesh.mesh.indices.size() % 3 == 0);
        // Vertices are local to their cell corner (apron tolerance: the
        // boundary cells of a chunk may overhang by a voxel).
        for (const render::MeshVertex& v : cellMesh.mesh.vertices) {
            CHECK(v.position.x > -3.0f);
            CHECK(v.position.x < 67.0f);
            CHECK(v.position.z > -3.0f);
            CHECK(v.position.z < 67.0f);
            CHECK(v.color.r >= 0.0f);
            CHECK(v.color.r <= 1.0f);
            CHECK(v.color.g >= 0.0f);
            CHECK(v.color.g <= 1.0f);
            CHECK(v.color.b >= 0.0f);
            CHECK(v.color.b <= 1.0f);
        }
    }

    // No two cell meshes may claim the same cell.
    for (size_t i = 0; i < r.cellMeshes.size(); ++i) {
        for (size_t j = i + 1; j < r.cellMeshes.size(); ++j) {
            const bool same = r.cellMeshes[i].cx == r.cellMeshes[j].cx &&
                              r.cellMeshes[i].cz == r.cellMeshes[j].cz;
            CHECK_FALSE(same);
        }
    }
}

TEST_CASE("dungeon bake: same seed replays bit for bit (§8)") {
    const DungeonBakeResult a = bakeDungeon(mineParams(7));
    const DungeonBakeResult b = bakeDungeon(mineParams(7));
    REQUIRE_FALSE(a.empty());
    REQUIRE(a.cellMeshes.size() == b.cellMeshes.size());
    for (size_t m = 0; m < a.cellMeshes.size(); ++m) {
        const render::MeshData& ma = a.cellMeshes[m].mesh;
        const render::MeshData& mb = b.cellMeshes[m].mesh;
        REQUIRE(ma.vertices.size() == mb.vertices.size());
        REQUIRE(ma.indices == mb.indices);
        for (size_t v = 0; v < ma.vertices.size(); ++v) {
            CHECK(ma.vertices[v].position == mb.vertices[v].position);
            CHECK(ma.vertices[v].color == mb.vertices[v].color);
        }
    }
    REQUIRE(a.torches.size() == b.torches.size());
    for (size_t t = 0; t < a.torches.size(); ++t) {
        CHECK(a.torches[t].position == b.torches[t].position);
    }
    CHECK(a.entrancePos == b.entrancePos);

    const DungeonBakeResult c = bakeDungeon(mineParams(8));
    CHECK(a.cellMeshes.size() * a.torches.size() !=
          c.cellMeshes.size() * c.torches.size());
}

TEST_CASE("dungeon bake: lock-and-key missions anchor paired barriers and levers") {
    DungeonParams p = mineParams(5);
    p.mission.patterns = { CyclePattern::SimpleLockKey };
    const DungeonBakeResult r = bakeDungeon(p);
    REQUIRE_FALSE(r.empty());

    REQUIRE_FALSE(r.barriers.empty());
    REQUIRE(r.levers.size() == r.barriers.size());
    for (const auto& barrier : r.barriers) {
        CHECK(barrier.lockId != 0);
        bool paired = false;
        for (const auto& lever : r.levers) {
            paired = paired || lever.lockId == barrier.lockId;
        }
        CHECK(paired);
    }
    REQUIRE(r.chests.size() == 1);
    const Vec3 goalFloor = roomCenter(r.space, r.space.goal);
    CHECK(glm::length(r.chests[0].position - goalFloor) < 0.01f);
    CHECK_FALSE(r.enemySpawns.empty()); // at least the goal guardian
}

TEST_CASE("dungeon bake: the exit door hugs the entrance room's outer wall") {
    const DungeonBakeResult r = bakeDungeon(mineParams(3));
    REQUIRE_FALSE(r.empty());
    const SpaceRoom& entrance = r.space.rooms[r.space.entrance];
    CHECK(entrance.pos.floor == 0);
    const Vec3 c = slotCenter(r.space.params, entrance.pos);
    // Against the border-side wall, inside the carved radius.
    CHECK(r.entrancePos.x < c.x - entrance.radius * 0.5f);
    CHECK(r.entrancePos.x > c.x - entrance.radius);
    CHECK(std::abs(r.entrancePos.z - c.z) < 0.01f);
    CHECK(r.entrancePos.y < 1.0f);
    CHECK(r.entrancePos.y > -1.0f);
}
