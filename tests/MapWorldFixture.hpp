#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>

#include "engine/platform/Paths.hpp"
#include "engine/render/landscape/TerrainNoise.hpp"
#include "engine/terrain/SandboxTerrain.hpp"
#include "engine/terrain/WaterBodies.hpp"
#include "engine/terrain/generation/TileBake.hpp"
#include "game/MapBaker.hpp"
#include "game/TerrainBakeStreamer.hpp"
#include "world/terrain/TerrainRegions.hpp"

// The BOUNDED MAP as the headless tests see it (docs/PAYSAGE.md §7.3,
// palier B): the production pipeline's output — every slice of one map
// read back from a map cache (baked on demand when missing), the map's
// overview as the fallback ground, lakes/rivers as the game publishes
// them — behind the same TerrainParams seam the game samples. The
// diagnostics measure THIS, never an isolated tile.

namespace maptest {

using render::terraingen::Lake;
using render::terraingen::River;
using render::terraingen::TileBakeParams;

struct MapWorld {
    TileBakeParams params; // tileSize = the slice size
    i32 mapX { 0 };
    i32 mapZ { 0 };
    i32 tilesPerSide { 1 };
    f32 mapSize { 0.0f };
    f32 minX { 0.0f }; // the map rect
    f32 minZ { 0.0f };
    f32 maxX { 0.0f };
    f32 maxZ { 0.0f };
    std::filesystem::path mapDir;
    render::terraingen::ProceduralControlParams controlParams;
    sptr<render::SandboxTerrain> sandbox;
    render::TerrainParams tp; // base = every slice, sandbox = overview
    render::WaterBodies bodies; // lakes with masks, sea level
    vector<Lake> lakes;
    vector<River> rivers;
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
    // Inside the map rect, `margin` meters away from the border lines
    // (kMapBorderMountainHalf = past the rim ranges).
    bool inside(f32 x, f32 z, f32 margin = 0.0f) const {
        return x >= minX + margin && x <= maxX - margin &&
               z >= minZ + margin && z <= maxZ - margin;
    }
};

// The game's bake params for `seed` (mirror of
// LandscapeScene::makeMapBakeParams with the landscape.toml defaults):
// borders on, sea level and recurve at their defaults.
inline TileBakeParams gameLikeParams(u32 seed = 1337) {
    TileBakeParams params;
    params.worldSeed = seed;
    params.controls.seed = seed;
    params.mapGrid.valid = true;
    return params;
}

// Where the hidden diagnostics find (or bake) their map: the
// MEADOWS_MAP_CACHE root when set (a terrain-cache/<seed> directory),
// else the game's own cache next to the test binary when it already
// holds a VALID map (mapX, mapZ) (the dev's baked map — no bake at all,
// and never a bake written into the game's directory), else a temp
// cache the test bakes into.
inline std::filesystem::path diagnosticsCacheRoot(u32 seed, i32 mapX = 0,
                                                  i32 mapZ = 0) {
    if (const char* env = std::getenv("MEADOWS_MAP_CACHE"); env && *env) {
        return std::filesystem::path { env };
    }
    const auto exe = platform::executableDir();
    const std::string seedDir = std::to_string(seed);
    for (const auto& candidate :
         { exe / ".." / "game" / "terrain-cache" / seedDir,
           exe / "terrain-cache" / seedDir }) {
        std::error_code ec;
        if (std::filesystem::is_directory(candidate, ec) &&
            game::mapBakedAndValid(candidate, mapX, mapZ,
                                   game::kMapTilesPerSide)) {
            return std::filesystem::weakly_canonical(candidate, ec);
        }
    }
    return std::filesystem::temp_directory_path() / "meadows-diag-cache" /
           seedDir;
}

// Loads map (mapX, mapZ) from `cacheRoot` (bakes it there first when
// its manifest is missing or stale, synchronously on the calling
// thread — minutes in Debug for a 24 km map, use the game's cache).
inline MapWorld loadOrBakeMap(const TileBakeParams& params, i32 mapX,
                              i32 mapZ, i32 tilesPerSide,
                              const std::filesystem::path& cacheRoot,
                              bool bakeIfMissing = true) {
    MapWorld world;
    world.params = params;
    world.mapX = mapX;
    world.mapZ = mapZ;
    world.tilesPerSide = tilesPerSide;
    world.mapSize = params.tileSize * static_cast<f32>(tilesPerSide);
    world.minX = static_cast<f32>(mapX) * world.mapSize;
    world.minZ = static_cast<f32>(mapZ) * world.mapSize;
    world.maxX = world.minX + world.mapSize;
    world.maxZ = world.minZ + world.mapSize;
    world.mapDir = game::mapCacheDir(cacheRoot, mapX, mapZ);
    world.controlParams = params.controls;
    world.controlParams.seed = params.worldSeed;

    if (!game::mapBakedAndValid(cacheRoot, mapX, mapZ, tilesPerSide)) {
        if (!bakeIfMissing) {
            return world;
        }
        std::error_code ec;
        std::filesystem::create_directories(cacheRoot, ec);
        game::bakeMap(params, mapX, mapZ, cacheRoot, nullptr,
                      tilesPerSide);
    }

    auto sandbox = std::make_shared<render::SandboxTerrain>();
    sandbox->controls = world.controlParams;
    sandbox->macro = params.macro;
    sandbox->grid.valid = params.mapGrid.valid;
    sandbox->grid.seed = params.worldSeed;
    sandbox->grid.mapSize = world.mapSize;
    sandbox->grid.seaLevel = params.macro.seaLevel;
    if (const auto overview = game::loadMapOverview(world.mapDir)) {
        sandbox->overviewGrid = overview->grid;
        sandbox->overview = overview->heights;
    }
    world.sandbox = sandbox;

    auto base = std::make_shared<render::TerrainBase>();
    world.bodies.seaLevel = params.macro.seaLevel;
    for (i32 dz = 0; dz < tilesPerSide; ++dz) {
        for (i32 dx = 0; dx < tilesPerSide; ++dx) {
            const i32 tx = mapX * tilesPerSide + dx;
            const i32 tz = mapZ * tilesPerSide + dz;
            const std::string stem =
                "tile_" + std::to_string(tx) + "_" + std::to_string(tz) +
                "_v" + std::to_string(render::terraingen::kTileBakeVersion);
            auto region = world::readTrgFile(world.mapDir / (stem + ".trg"));
            vector<Lake> lakes;
            vector<River> rivers;
            if (!region ||
                !game::readWaterFile(world.mapDir / (stem + ".twb"), lakes,
                                     rivers)) {
                continue;
            }
            region->detailAmplitude =
                render::terraingen::kRegionDetailAmplitude;
            region->detailWavelength =
                render::terraingen::kRegionDetailWavelength;
            region->detailOctaves = render::terraingen::kRegionDetailOctaves;
            base->regions.push_back(
                std::make_shared<render::TerrainRegion>(std::move(*region)));
            for (const Lake& lake : lakes) {
                render::LakeSurface surface;
                surface.level = lake.level;
                surface.minX = lake.minX;
                surface.minZ = lake.minZ;
                surface.maxX = lake.maxX;
                surface.maxZ = lake.maxZ;
                surface.maskWidth = lake.maskWidth;
                surface.maskHeight = lake.maskHeight;
                surface.maskTexel = lake.maskTexel;
                surface.mask = lake.mask;
                world.bodies.lakes.push_back(std::move(surface));
            }
            world.lakes.insert(world.lakes.end(), lakes.begin(),
                               lakes.end());
            world.rivers.insert(world.rivers.end(), rivers.begin(),
                                rivers.end());
            ++world.slicesLoaded;
        }
    }
    world.tp.seed = params.worldSeed;
    world.tp.base = base;
    world.tp.sandbox = sandbox;
    return world;
}

// FNV-1a over the raw height bytes of every loaded slice, in slice
// order: the map's content hash (an optimization that moves it changed
// the terrain; a tuning brick moves it by design and re-pins it).
inline u64 heightsHash(const MapWorld& world) {
    u64 h = 1469598103934665603ull;
    for (const auto& region : world.tp.base->regions) {
        const auto* bytes = reinterpret_cast<const unsigned char*>(
            region->heights.data());
        const size_t size = region->heights.size() * sizeof(f32);
        for (size_t i = 0; i < size; ++i) {
            h ^= bytes[i];
            h *= 1099511628211ull;
        }
    }
    return h;
}

} // namespace maptest
