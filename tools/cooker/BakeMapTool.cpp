#include "BakeMapTool.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include "data/forms/LandscapeForms.hpp"
#include "data/plugins/EditSession.hpp"
#include "data/plugins/PluginConfig.hpp"
#include "data/plugins/Resolver.hpp"
#include "data/plugins/TomlWriter.hpp"
#include "engine/core/Jobs.hpp"
#include "engine/core/Log.hpp"
#include "engine/terrain/TerrainBase.hpp"
#include "game/AllForms.hpp"
#include "game/MapBaker.hpp"
#include "game/TerrainBakeStreamer.hpp"
#include "world/terrain/MapRecords.hpp"
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
    const bool tilesGiven = argc >= 6 && argv[5][0] != '-';
    const i32 tilesPerSide =
        tilesGiven ? std::atoi(argv[5]) : game::kMapTilesPerSide;
    if (tilesPerSide < 2 || tilesPerSide > 8) {
        LOG_ERROR("bake-map: tilesPerSide must be 2-8");
        return 1;
    }
    // Optional flags after tilesPerSide: "--" disables the border
    // transitions (calibration bakes); "--export-plugin <name>" also
    // emits the map as an ordinary §5 plugin (M5.2).
    bool borders = true;
    const char* exportName = nullptr;
    for (int i = 5; i < argc; ++i) {
        if (std::strcmp(argv[i], "--") == 0) {
            borders = false;
        } else if (std::strcmp(argv[i], "--export-plugin") == 0 &&
                   i + 1 < argc) {
            exportName = argv[++i];
        }
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
    params.mapGrid.valid = borders; // MapBaker fills the spec

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

    // Mirror calibration report: mean/max (baked - shaped analytic)
    // per 100 m analytic band — the data that refits the
    // macroHeightAnalytic compression (its constants were fitted to
    // the WINDOWED bake; a drifted mirror pops the ground when slices
    // land and draws a long straight step at the streaming frontier).
    {
        const render::terraingen::ProceduralControls controls {
            [&] {
                render::terraingen::ProceduralControlParams cp =
                    params.controls;
                cp.seed = params.worldSeed;
                return cp;
            }()
        };
        render::terraingen::MapGridSpec gridSpec = params.mapGrid;
        if (gridSpec.valid) {
            gridSpec.seed = params.worldSeed;
            gridSpec.mapSize =
                tileSize * static_cast<f32>(tilesPerSide);
            gridSpec.seaLevel = params.macro.seaLevel;
        }
        const auto overview = game::loadMapOverview(mapDir);
        const auto fallbackAt = [&](f32 x, f32 z, f32 analytic) {
            if (!overview) {
                return analytic;
            }
            const auto& g = overview->grid;
            const f32 u = (x - g.originX) / g.texelSize;
            const f32 v = (z - g.originZ) / g.texelSize;
            if (u < 0.0f || v < 0.0f ||
                u > static_cast<f32>(g.n - 1) ||
                v > static_cast<f32>(g.n - 1)) {
                return analytic;
            }
            const u32 c0 = glm::min(static_cast<u32>(u), g.n - 2);
            const u32 r0 = glm::min(static_cast<u32>(v), g.n - 2);
            const f32 tu = u - static_cast<f32>(c0);
            const f32 tv = v - static_cast<f32>(r0);
            const auto at = [&](u32 c, u32 r) {
                return overview
                    ->heights[static_cast<size_t>(r) * g.n + c];
            };
            return glm::mix(
                glm::mix(at(c0, r0), at(c0 + 1, r0), tu),
                glm::mix(at(c0, r0 + 1), at(c0 + 1, r0 + 1), tu), tv);
        };
        LOG_INFO("bake-map: mirror report vs {}",
                 overview ? "the 64 m overview" : "the shaped analytic");
        constexpr u32 kBands = 12;
        f64 sum[kBands] = {};
        f32 worstBand[kBands] = {};
        u32 n[kBands] = {};
        for (i32 dz = 0; dz < tilesPerSide; ++dz) {
            for (i32 dx = 0; dx < tilesPerSide; ++dx) {
                const auto slice =
                    readSlice(mapDir, tx0 + dx, tz0 + dz);
                if (!slice) {
                    continue;
                }
                for (f32 z = slice->originZ + 128.0f;
                     z < slice->originZ + slice->spanZ() - 128.0f;
                     z += 96.0f) {
                    for (f32 x = slice->originX + 128.0f;
                         x < slice->originX + slice->spanX() - 128.0f;
                         x += 96.0f) {
                        const f32 fallback = fallbackAt(
                            x, z,
                            render::terraingen::applyMapGridShape(
                                controls, params.macro, gridSpec, x, z,
                                render::terraingen::
                                    macroHeightAnalytic(
                                        controls, params.macro, x,
                                        z)));
                        const f32 rel =
                            fallback - params.macro.seaLevel;
                        if (rel < 0.0f) {
                            continue; // the sea agrees by decree
                        }
                        const u32 band = glm::min(
                            kBands - 1,
                            static_cast<u32>(rel / 100.0f));
                        const f32 delta =
                            render::terrain::baseHeight(*slice, x,
                                                        z) -
                            fallback;
                        sum[band] += delta;
                        worstBand[band] = glm::max(
                            worstBand[band], std::abs(delta));
                        ++n[band];
                    }
                }
            }
        }
        for (u32 b = 0; b < kBands; ++b) {
            if (n[b] == 0) {
                continue;
            }
            LOG_INFO("bake-map: mirror band {:>4}-{:<4} m — mean "
                     "delta {:+7.1f} m, max |delta| {:6.1f} m ({} "
                     "samples)",
                     b * 100, (b + 1) * 100,
                     sum[b] / static_cast<f64>(n[b]), worstBand[b],
                     n[b]);
        }
    }
    // --export-plugin (M5.2): the baked map as an ORDINARY §5 mod —
    // slices copied under data/mods/terrain/, records staged through
    // stageMapRecords, one TOML out. Deterministic guids (map guid =
    // f(seed, coords)) mean a re-export PATCHES the same world.
    if (exportName) {
        const core::Guid mapGuid = world::mapWorldspaceGuid(
            params.worldSeed, mapX, mapZ);
        const auto modsDir = gameDir / "data" / "mods";
        char rel[64];
        std::snprintf(rel, sizeof(rel), "terrain/map_%d_%d", mapX,
                      mapZ);
        std::error_code errc;
        std::filesystem::create_directories(modsDir / rel, errc);

        vector<world::MapSliceRecord> slices;
        vector<data::AssetEntry> assetEntries;
        vector<render::terraingen::Lake> lakes;
        vector<render::terraingen::River> rivers;
        for (i32 dz = 0; dz < tilesPerSide; ++dz) {
            for (i32 dx = 0; dx < tilesPerSide; ++dx) {
                const i32 tx = tx0 + dx;
                const i32 tz = tz0 + dz;
                char stem[64];
                std::snprintf(stem, sizeof(stem), "tile_%d_%d_v%u",
                              tx, tz,
                              render::terraingen::kTileBakeVersion);
                const auto trg = mapDir / (str { stem } + ".trg");
                const auto dest = modsDir / rel / (str { stem } + ".trg");
                std::filesystem::copy_file(
                    trg, dest,
                    std::filesystem::copy_options::overwrite_existing,
                    errc);
                if (errc) {
                    LOG_ERROR("bake-map: cannot copy {} to the mod",
                              trg.string());
                    return 1;
                }
                const u32 index = static_cast<u32>(
                    dz * tilesPerSide + dx);
                const core::Guid asset =
                    world::mapSliceAssetGuid(mapGuid, index);
                slices.push_back(
                    { tx, tz, asset,
                      render::terraingen::kRegionDetailAmplitude,
                      render::terraingen::kRegionDetailWavelength,
                      render::terraingen::kRegionDetailOctaves });
                assetEntries.push_back(
                    { asset,
                      str { rel } + "/" + stem + ".trg" });
                vector<render::terraingen::Lake> sliceLakes;
                vector<render::terraingen::River> sliceRivers;
                if (game::readWaterFile(mapDir / (str { stem } + ".twb"),
                                        sliceLakes, sliceRivers)) {
                    for (auto& lake : sliceLakes) {
                        lakes.push_back(std::move(lake));
                    }
                    for (auto& river : sliceRivers) {
                        rivers.push_back(std::move(river));
                    }
                }
            }
        }

        char mapName[64];
        std::snprintf(mapName, sizeof(mapName), "Map_%d_%d", mapX,
                      mapZ);
        data::FormDatabase stageDb;
        data::EditSession session { stageDb, formTypes };
        world::stageMapRecords(session, stageDb, mapGuid, mapName,
                               mapX, mapZ,
                               params.tileSize *
                                   static_cast<f32>(tilesPerSide),
                               params.worldSeed, slices, lakes,
                               rivers, 48.0f);
        data::Plugin plugin = session.exportPlugin(
            core::Guid::combine(mapGuid,
                                core::Guid { 1, 0x6d6170706c756731ull }),
            exportName);
        plugin.assets = std::move(assetEntries);
        const auto pluginPath =
            modsDir / (str { exportName } + ".toml");
        std::ofstream file { pluginPath, std::ios::trunc };
        if (!file) {
            LOG_ERROR("bake-map: cannot write {}",
                      pluginPath.string());
            return 1;
        }
        file << data::writePluginToml(plugin, formTypes);
        LOG_INFO("bake-map: exported plugin {} ({} records, {} "
                 "assets) — add 'mods/{}.toml' to plugins.toml",
                 pluginPath.string(), plugin.records.size(),
                 plugin.assets.size(), exportName);
    }
    return worst <= 5.0f ? 0 : 1;
}

} // namespace cooker
