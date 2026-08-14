#pragma once

#include <filesystem>
#include <optional>

#include <glm/glm.hpp>

#include "engine/core/Defines.hpp"
#include "engine/dungeon/MeshExtract.hpp"

// Stage D5 of the dungeon pipeline (docs/DUNGEON-GEN.md) — the walkable
// grid, baked from the same density field as the mesh so the two cannot
// disagree. Multi-level by design: one XZ column holds SEVERAL floors
// (stacked galleries, a balcony over a hall) — the exact thing a heightmap
// navigator cannot express. Storage is CSR: per column, a slice into a flat
// level array, each level a floor height plus head clearance.
//
// Serialized as `.nvg` (magic + versions, the .trg/.cmesh convention); the
// scene loads it to build a world::InteriorNavigator.

namespace dungeon {

struct NavGrid {
    f32 originX { 0.0f };
    f32 originZ { 0.0f };
    f32 cellSize { 0.5f };
    u32 width { 0 };
    u32 depth { 0 };

    struct Level {
        f32 floorY { 0.0f };
        f32 clearance { 0.0f };
    };
    vector<u32> firstLevel; // width * depth + 1 entries (CSR row starts)
    vector<Level> levels;   // sorted by floorY within each column

    bool empty() const { return levels.empty(); }
    u32 columnOf(f32 x, f32 z) const; // ~0u when outside the grid
    // Levels of a column as [begin, end) indices into `levels`.
    void columnLevels(u32 column, u32& begin, u32& end) const;

    // Standing air around foot height y in column (ix, iz): some floor at
    // most a step below with headroom above. The building block of the
    // agent-radius story below.
    bool airAt(i32 ix, i32 iz, f32 y) const;
    // The column is walkable but a neighbouring column is rock at this
    // height — the cell hugs a wall. The grid stays permissive (narrow
    // tunnels must remain traversable), so wall adjacency is a PENALTY
    // for the pathfinder and a veto for actor SPAWN anchors, never an
    // erosion of the grid itself.
    bool wallAdjacent(i32 ix, i32 iz, f32 y) const;
};

// Scans every column of [min, max] bottom-up for solid->air crossings with
// enough headroom. Deterministic for (density, bounds, params).
NavGrid bakeNavGrid(const DensityFn& density, const Vec3& min, const Vec3& max,
                    f32 cellSize, f32 minClearance);

bool writeNvgFile(const std::filesystem::path& path, const NavGrid& grid,
                  u32 contentVersion);
std::optional<NavGrid> readNvgFile(const std::filesystem::path& path,
                                   u32 expectedContentVersion);

} // namespace dungeon
