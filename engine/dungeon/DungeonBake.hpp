#pragma once

#include <glm/glm.hpp>

#include "engine/assets/MeshData.hpp"
#include "engine/core/Defines.hpp"
#include "engine/dungeon/DensityField.hpp"
#include "engine/dungeon/MissionGraph.hpp"
#include "engine/dungeon/NavGrid.hpp"
#include "engine/dungeon/SpaceGraph.hpp"

// Stage D3+D4+D6 orchestration of the dungeon pipeline (docs/DUNGEON-GEN.md):
// one pure, deterministic call from a seed to CPU data ready for records —
// cavern meshes chunked per 64 m cell (vertices LOCAL to the cell corner, the
// reference transform re-bases them), wall torch anchors, the entrance frame.
// Vertex colors are baked here: rock tint x low-frequency variation x AO
// sampled directly in the density field (hemisphere probes — never
// rays-vs-triangles, which would be O(V*T) on cavern-sized chunks).
// Runs headless: JobSystem workers or doctests, never the render thread.

namespace dungeon {

constexpr u32 kDungeonBakeVersion = 1;

struct DungeonParams {
    u32 seed { 1337 };
    MissionParams mission;   // seeds inside are overridden from `seed`
    SpaceParams space;
    DensityParams density;
    f32 cellSize { 64.0f };  // must match the interior WorldspaceForm
    f32 voxelSize { 0.8f };
    f32 torchSpacing { 13.0f };
    f32 navCellSize { 0.5f };
    f32 navMinClearance { 1.8f };
    // Population density (deterministic per seed): plain rooms roll an
    // enemy and possibly a bonus ore vein; dead-end rooms ALWAYS hold a
    // vein (the mine's lodes — what dead ends are for).
    f64 enemyChancePerRoom { 0.45 };
    f64 bonusVeinChancePerRoom { 0.25 };
    // No per-chunk decimation: assets::simplifyMesh does not lock borders,
    // so it would crack the chunk seams. If profiling demands it, the named
    // extension is border-locked simplification (meshopt_SimplifyLockBorder).
};

struct DungeonBakeResult {
    MissionGraph mission;
    SpaceGraph space;

    struct CellMesh {
        i32 cx { 0 };
        i32 cz { 0 };
        render::MeshData mesh; // vertices local to the cell corner
    };
    vector<CellMesh> cellMeshes;

    NavGrid navGrid; // stage D5, baked from the same field as the meshes

    struct Torch {
        Vec3 position;   // on the tunnel wall, world (interior) coordinates
        Vec3 wallNormal; // out of the wall, toward the air
    };
    vector<Torch> torches;

    // Gameplay anchors, derived from the mission semantics (floors are flat
    // planes, so y is exact). lockId pairs a barrier with its lever.
    // width widens the prop across the tunnel: a barrier on a corridor
    // TURN spans the junction's diagonal, wider than a straight tube.
    struct Anchor {
        Vec3 position;
        f32 yawDeg { 0.0f };
        u32 lockId { 0 };
        f32 width { 1.0f };
    };
    vector<Anchor> barriers;    // across each Locked corridor (lockId set)
    vector<Anchor> levers;      // in the matching Key room (lockId set)
    vector<Anchor> chests;      // the Goal prize
    vector<Anchor> oreVeins;    // Reward rooms (sub-cycle goals)
    vector<Anchor> enemySpawns; // along Dangerous arcs + one Goal guardian
    vector<Anchor> npcSpawns;   // flavor NPC, near the first ore vein
    vector<Anchor> patrolPoints; // plain-room floors: NPC wander anchors

    Vec3 entrancePos { 0.0f }; // floor point of the entrance room
    Vec3 entranceDir { -1.0f, 0.0f, 0.0f }; // toward the outside door
    Vec3 boundsMin { 0.0f }; // carved volume bounds (killZ / buriedBelowY)
    Vec3 boundsMax { 0.0f };
    f32 cellSize { 64.0f }; // the chunking the cell meshes were cut on

    bool empty() const { return cellMeshes.empty(); }
};

// Deterministic for (params): same seed -> same result, bit for bit.
DungeonBakeResult bakeDungeon(const DungeonParams& params);

} // namespace dungeon
