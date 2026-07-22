#pragma once

#include <unordered_map>

#include "engine/core/Defines.hpp"

// Authored-terrain delta-overlay data. Headless data structs
// shared by the terrain noise/height code (engine/render/landscape/TerrainNoise)
// and the world authoring layer (world/terrain/TerrainPatches) that BUILDS them
// from TerrainPatchForm records. Extracted to this headless home so world/ needs
// no dependency on engine/render/ (§2.10). Namespace kept as `render` to avoid
// churning the many height consumers — these are plain data, not renderer types.

namespace render {

// Per-chunk DELTA grid added on top of the procedural base — final height =
// noise + bilinear(delta). Sculpting edits grids then publishes a NEW immutable
// instance, so workers holding the old pointer stay race-free.
struct HeightPatch {
    u32 samples { 0 }; // n: the grid is n x n, row-major, x fastest,
                       // rows along +Z; edge samples are SHARED with the
                       // neighbouring chunk (seamless by construction)
    vector<f32> deltas; // meters, n * n
};

struct HeightPatches {
    f32 chunkSize { 64.0f }; // world meters per chunk (= terrain chunks)
    std::unordered_map<u64, HeightPatch> chunks;

    static u64 keyOf(i32 cx, i32 cz) {
        return (static_cast<u64>(static_cast<u32>(cx)) << 32) |
               static_cast<u64>(static_cast<u32>(cz));
    }
};

} // namespace render
