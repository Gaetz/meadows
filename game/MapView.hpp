#pragma once

#include <filesystem>

#include "engine/render/landscape/TerrainNoise.hpp"
#include "engine/terrain/SandboxTerrain.hpp"
#include "engine/terrain/WaterBodies.hpp"
#include "engine/terrain/generation/TileBake.hpp"

// A baked bounded map read back from its cache, behind the same
// TerrainParams seam the game samples (docs/PAYSAGE.md §1.2): the
// published ground of every slice, the overview as the fallback, the
// lakes with their masks. The cooker's landscape report and the
// headless instruments measure THIS, never an isolated tile.

namespace game {

struct MapView {
    render::terraingen::TileBakeParams params;
    i32 mapX { 0 };
    i32 mapZ { 0 };
    i32 tilesPerSide { 1 };
    f32 mapSize { 0.0f };
    f32 minX { 0.0f }; // the map rect
    f32 minZ { 0.0f };
    f32 maxX { 0.0f };
    f32 maxZ { 0.0f };
    std::filesystem::path mapDir;
    sptr<render::SandboxTerrain> sandbox;
    render::TerrainParams tp; // base = every slice, sandbox = overview
    render::WaterBodies bodies;
    vector<render::terraingen::Lake> lakes;
    vector<render::terraingen::River> rivers;
    u32 slicesLoaded { 0 };

    f32 seaLevel() const { return params.macro.seaLevel; }
    f32 centreX() const { return 0.5f * (minX + maxX); }
    f32 centreZ() const { return 0.5f * (minZ + maxZ); }
    // The published ground (bicubic slices + detail), like the game.
    f32 height(f32 x, f32 z) const {
        return render::terrain::height(tp, x, z);
    }
    // The overview ground (64 m, eroded) — cheap, map-wide.
    f32 overviewHeight(f32 x, f32 z) const {
        return render::sandboxFallbackHeight(*sandbox, x, z);
    }
    bool inside(f32 x, f32 z, f32 margin = 0.0f) const {
        return x >= minX + margin && x <= maxX - margin &&
               z >= minZ + margin && z <= maxZ - margin;
    }
    // Water at (x, z) (a lake or the sea), as the spawn probe sees it.
    bool wet(f32 x, f32 z) const;
    // The game's start on this map (probeMapSpawn with the wet oracle;
    // the centre when nothing dry is found).
    Vec3 spawn() const;
};

// Loads map (mapX, mapZ) from `cacheRoot`: nullopt when its manifest is
// missing or stale for `params` (the caller bakes first).
std::optional<MapView> loadMapView(
    const render::terraingen::TileBakeParams& params, i32 mapX, i32 mapZ,
    i32 tilesPerSide, const std::filesystem::path& cacheRoot);

} // namespace game
