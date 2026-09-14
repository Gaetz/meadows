#include "BakeMapTool.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "data/forms/LandscapeForms.hpp"
#include "data/plugins/PluginConfig.hpp"
#include "data/plugins/Resolver.hpp"
#include "engine/core/Jobs.hpp"
#include "engine/core/Log.hpp"
#include "engine/terrain/TerrainBase.hpp"
#include "game/AllForms.hpp"
#include "game/MapBaker.hpp"
#include "world/terrain/TerrainRegions.hpp"

namespace cooker {

namespace {

// Max |baseHeight(A) - baseHeight(B)| over the shared band of two
// adjacent slices (the map-proto metric — the chantier's acceptance
// number).
f32 bandDivergence(const render::TerrainRegion& ra,
                   const render::TerrainRegion& rb, bool axisZ,
                   f32 border, f32 alongMin, f32 alongMax, f32 margin) {
    f32 bandMax = 0.0f;
    for (f32 d = -margin + 2.0f; d <= margin - 2.0f; d += 2.0f) {
        for (f32 s = alongMin + 2.0f; s < alongMax; s += 4.0f) {
            const f32 x = axisZ ? s : border + d;
            const f32 z = axisZ ? border + d : s;
            const f32 ha = render::terrain::baseHeight(ra, x, z);
            const f32 hb = render::terrain::baseHeight(rb, x, z);
            bandMax = glm::max(bandMax, std::abs(ha - hb));
        }
    }
    return bandMax;
}

std::optional<render::TerrainRegion> readSlice(
    const std::filesystem::path& mapDir, i32 tx, i32 tz) {
    char stem[64];
    std::snprintf(stem, sizeof(stem), "tile_%d_%d_v%u.trg", tx, tz,
                  render::terraingen::kTileBakeVersion);
    return world::readTrgFile(mapDir / stem);
}

} // namespace

int bakeMapCmd(char** argv, int argc) {
    const std::filesystem::path gameDir = argv[2];
    const i32 mapX = std::atoi(argv[3]);
    const i32 mapZ = std::atoi(argv[4]);
    const i32 tilesPerSide =
        argc >= 6 ? std::atoi(argv[5]) : game::kMapTilesPerSide;
    if (tilesPerSide < 2 || tilesPerSide > 8) {
        LOG_ERROR("bake-map: tilesPerSide must be 2-8");
        return 1;
    }
    // Edge styles, one char per side N/E/S/W: s = sea, r = ridges,
    // - = no rim at all. Default: an island (sea everywhere).
    const char* edges = argc >= 7 ? argv[6] : "ssss";
    render::terraingen::MapEdgeSpec edge;
    if (std::strlen(edges) == 4 && std::strcmp(edges, "----") != 0) {
        const auto style = [](char c) {
            return c == 'r' ? render::terraingen::MapEdgeStyle::Ridges
                            : render::terraingen::MapEdgeStyle::Sea;
        };
        edge.valid = true;
        edge.north = style(edges[0]);
        edge.east = style(edges[1]);
        edge.south = style(edges[2]);
        edge.west = style(edges[3]);
    }

    // Game bake params (the pre-bake resolution path: the SAME plugin-
    // resolved tuning the game bakes with).
    const auto dataDir = gameDir / "data";
    data::FormTypeRegistry formTypes;
    game::registerAllFormTypes(formTypes);
    data::PluginConfig pluginConfig;
    if (const auto loaded =
            data::loadPluginConfigFile(dataDir / "plugins.toml")) {
        pluginConfig = *loaded;
    } else {
        pluginConfig = data::defaultConfigFromDirectory(dataDir / "base");
        for (auto& entry : pluginConfig.entries) {
            entry.file = "base/" + entry.file;
        }
    }
    data::PluginStack stack =
        data::loadPluginStack(dataDir, pluginConfig, formTypes);
    if (stack.plugins.empty()) {
        LOG_ERROR("bake-map: no plugins under {} — wrong gameDir?",
                  dataDir.string());
        return 1;
    }
    data::FormDatabase forms;
    data::resolve(data::pointersOf(stack), formTypes, forms);
    const data::LandscapeTuningForm tuning =
        data::resolveLandscapeTuning(forms);

    render::terraingen::TileBakeParams params;
    params.worldSeed = tuning.terrainSeed;
    params.controls.seed = tuning.terrainSeed;
    params.macro.seaLevel = tuning.seaLevel;
    params.macro.recurveLow = tuning.terrainRecurveLow;
    params.macro.recurveMid = tuning.terrainRecurveMid;
    params.macro.recurveHigh = tuning.terrainRecurveHigh;
    params.mapEdge = edge; // styles only; MapBaker fills the rect

    const auto cacheDir = gameDir / "terrain-cache" /
                          std::to_string(tuning.terrainSeed);
    LOG_INFO("bake-map: seed {} | map ({}, {}) = {}x{} slices "
             "({:.0f} m)",
             params.worldSeed, mapX, mapZ, tilesPerSide, tilesPerSide,
             params.tileSize * static_cast<f32>(tilesPerSide));

    core::JobSystem jobs;
    const game::MapBakeStats stats = game::bakeMap(
        params, mapX, mapZ, cacheDir, &jobs, tilesPerSide,
        [](u32 landed, u32 total) {
            LOG_INFO("bake-map: [{}/{}] slice landed", landed, total);
        });
    if (stats.slicesWritten !=
        static_cast<u32>(tilesPerSide) * static_cast<u32>(tilesPerSide)) {
        LOG_ERROR("bake-map: incomplete bake");
        return 1;
    }

    // Interior-border acceptance sweep, read back from the WRITTEN
    // slices (two resident at a time — memory-bounded).
    const auto mapDir = game::mapCacheDir(cacheDir, mapX, mapZ);
    const f32 tileSize = params.tileSize;
    const f32 margin = params.overlapMargin;
    const i32 tx0 = mapX * tilesPerSide;
    const i32 tz0 = mapZ * tilesPerSide;
    f32 worst = 0.0f;
    for (i32 dz = 0; dz < tilesPerSide; ++dz) {
        for (i32 dx = 0; dx < tilesPerSide; ++dx) {
            const i32 tx = tx0 + dx;
            const i32 tz = tz0 + dz;
            const auto a = readSlice(mapDir, tx, tz);
            if (!a) {
                LOG_ERROR("bake-map: cannot re-read slice ({}, {})",
                          tx, tz);
                return 1;
            }
            if (dx + 1 < tilesPerSide) {
                if (const auto b = readSlice(mapDir, tx + 1, tz)) {
                    const f32 alongMin =
                        static_cast<f32>(tz) * tileSize;
                    const f32 band = bandDivergence(
                        *a, *b, false,
                        static_cast<f32>(tx + 1) * tileSize, alongMin,
                        alongMin + tileSize, margin);
                    if (band > 2.0f) {
                        LOG_INFO("bake-map: border x={:.0f} z=[{:.0f}.."
                                 "{:.0f}] — band max {:.3f} m",
                                 static_cast<f32>(tx + 1) * tileSize,
                                 alongMin, alongMin + tileSize, band);
                    }
                    worst = glm::max(worst, band);
                }
            }
            if (dz + 1 < tilesPerSide) {
                if (const auto b = readSlice(mapDir, tx, tz + 1)) {
                    const f32 alongMin =
                        static_cast<f32>(tx) * tileSize;
                    const f32 band = bandDivergence(
                        *a, *b, true,
                        static_cast<f32>(tz + 1) * tileSize, alongMin,
                        alongMin + tileSize, margin);
                    if (band > 2.0f) {
                        LOG_INFO("bake-map: border z={:.0f} x=[{:.0f}.."
                                 "{:.0f}] — band max {:.3f} m",
                                 static_cast<f32>(tz + 1) * tileSize,
                                 alongMin, alongMin + tileSize, band);
                    }
                    worst = glm::max(worst, band);
                }
            }
        }
    }
    // With the shared surface AND the shared map hydrology, the only
    // per-slice pass left is the fine erosion (support-bounded, max
    // depth 3.5 m) plus the ±1.1 m relief damp — the gate is their
    // stack (docs/TERRAIN-MAPS.md; windowed tiles measured 200-441 m).
    LOG_INFO("bake-map: worst interior band divergence {:.3f} m "
             "(acceptance <= 5 m — fine-erosion residual)",
             worst);
    return worst <= 5.0f ? 0 : 1;
}

} // namespace cooker
