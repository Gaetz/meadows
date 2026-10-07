#include "game/MapView.hpp"

#include <string>

#include "engine/terrain/WaterQuery.hpp"
#include "game/MapBaker.hpp"
#include "game/TerrainBakeStreamer.hpp"
#include "world/terrain/TerrainRegions.hpp"

namespace game {

using render::terraingen::Lake;
using render::terraingen::River;
using render::terraingen::TileBakeParams;

bool MapView::wet(f32 x, f32 z) const {
    const f32 h = height(x, z);
    if (h < seaLevel() + 0.5f) {
        return true;
    }
    return render::terrain::waterSurfaceAt(bodies, x, z, h + 1.0f)
        .has_value();
}

Vec3 MapView::spawn() const {
    const auto wetAt = [this](f32 x, f32 z) { return wet(x, z); };
    if (const auto spot = render::probeMapSpawn(*sandbox, mapX, mapZ,
                                                seaLevel(), wetAt)) {
        return *spot;
    }
    return { centreX(), overviewHeight(centreX(), centreZ()), centreZ() };
}

std::optional<MapView> loadMapView(const TileBakeParams& params, i32 mapX,
                                   i32 mapZ, i32 tilesPerSide,
                                   const std::filesystem::path& cacheRoot) {
    if (!mapBakedAndValid(cacheRoot, mapX, mapZ, tilesPerSide, &params)) {
        return std::nullopt;
    }
    MapView view;
    view.params = params;
    view.mapX = mapX;
    view.mapZ = mapZ;
    view.tilesPerSide = tilesPerSide;
    view.mapSize = params.tileSize * static_cast<f32>(tilesPerSide);
    view.minX = static_cast<f32>(mapX) * view.mapSize;
    view.minZ = static_cast<f32>(mapZ) * view.mapSize;
    view.maxX = view.minX + view.mapSize;
    view.maxZ = view.minZ + view.mapSize;
    view.mapDir = mapCacheDir(cacheRoot, mapX, mapZ);

    auto sandbox = std::make_shared<render::SandboxTerrain>();
    sandbox->controls = params.controls;
    sandbox->controls.seed = params.worldSeed;
    sandbox->macro = params.macro;
    sandbox->grid.valid = params.mapGrid.valid;
    sandbox->grid.seed = params.worldSeed;
    sandbox->grid.mapSize = view.mapSize;
    sandbox->grid.seaLevel = params.macro.seaLevel;
    if (const auto overview = loadMapOverview(view.mapDir)) {
        sandbox->overviewGrid = overview->grid;
        sandbox->overview = overview->heights;
    }
    view.sandbox = sandbox;

    auto base = std::make_shared<render::TerrainBase>();
    view.bodies.seaLevel = params.macro.seaLevel;
    for (i32 dz = 0; dz < tilesPerSide; ++dz) {
        for (i32 dx = 0; dx < tilesPerSide; ++dx) {
            const i32 tx = mapX * tilesPerSide + dx;
            const i32 tz = mapZ * tilesPerSide + dz;
            const std::string stem =
                "tile_" + std::to_string(tx) + "_" + std::to_string(tz) +
                "_v" + std::to_string(render::terraingen::kTileBakeVersion);
            auto region = world::readTrgFile(view.mapDir / (stem + ".trg"));
            vector<Lake> lakes;
            vector<River> rivers;
            if (!region ||
                !readWaterFile(view.mapDir / (stem + ".twb"), lakes,
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
                view.bodies.lakes.push_back(std::move(surface));
            }
            view.lakes.insert(view.lakes.end(), lakes.begin(), lakes.end());
            view.rivers.insert(view.rivers.end(), rivers.begin(),
                               rivers.end());
            ++view.slicesLoaded;
        }
    }
    view.tp.seed = params.worldSeed;
    view.tp.base = base;
    view.tp.sandbox = sandbox;
    return view;
}

} // namespace game
