#include "MapProtoTool.hpp"

#include <chrono>
#include <cstdlib>
#include <map>

#include "data/forms/LandscapeForms.hpp"
#include "data/plugins/PluginConfig.hpp"
#include "data/plugins/Resolver.hpp"
#include "engine/core/Log.hpp"
#include "engine/terrain/TerrainBase.hpp"
#include "engine/terrain/generation/TileBake.hpp"
#include "game/AllForms.hpp"

namespace cooker {

namespace {

using namespace render::terraingen;

f64 secondsSince(const std::chrono::steady_clock::time_point& start) {
    return std::chrono::duration<f64> { std::chrono::steady_clock::now() -
                                        start }
        .count();
}

// Band divergence between two regions sharing a border (the
// border-report metric): max |baseHeight(A) - baseHeight(B)| over the
// shared +-overlapMargin band.
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

} // namespace

int mapProto(char** argv, int argc) {
    const std::filesystem::path gameDir = argv[2];
    const i32 mapTx = std::atoi(argv[3]);
    const i32 mapTz = std::atoi(argv[4]);
    const i32 tilesPerSide = argc >= 6 ? std::atoi(argv[5]) : 2;
    if (tilesPerSide < 2 || tilesPerSide > 6) {
        LOG_ERROR("map-proto: tilesPerSide must be 2-6");
        return 1;
    }

    // Game bake params (the pre-bake resolution path).
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
        LOG_ERROR("map-proto: no plugins under {} — wrong gameDir?",
                  dataDir.string());
        return 1;
    }
    data::FormDatabase forms;
    data::resolve(data::pointersOf(stack), formTypes, forms);
    const data::LandscapeTuningForm tuning =
        data::resolveLandscapeTuning(forms);

    TileBakeParams params;
    params.worldSeed = tuning.terrainSeed;
    params.controls.seed = tuning.terrainSeed;
    params.macro.seaLevel = tuning.seaLevel;
    params.macro.recurveLow = tuning.terrainRecurveLow;
    params.macro.recurveMid = tuning.terrainRecurveMid;
    params.macro.recurveHigh = tuning.terrainRecurveHigh;

    const f32 tileSize = params.tileSize;
    const f32 mapSize = tileSize * static_cast<f32>(tilesPerSide);
    // The map stage-1: same recipe, map-sized window. The margin must
    // cover stage-2's widest composite request (the canonical-basin
    // window, tile + kBasinResolveMargin) for every tile of the map.
    TileBakeParams mapParams = params;
    mapParams.tileSize = mapSize;
    mapParams.apron = kBasinResolveMargin;

    LOG_INFO("map-proto: seed {} | map ({}, {}) = {}x{} tiles "
             "({:.0f} m), stage-1 window {:.0f} m at {:.0f} m texels",
             params.worldSeed, mapTx, mapTz, tilesPerSide, tilesPerSide,
             mapSize, mapSize + 2.0f * mapParams.apron,
             mapParams.macroTexel);

    const auto s1Start = std::chrono::steady_clock::now();
    const TileStage1 mapS1 = bakeTileStage1(mapParams, mapTx, mapTz);
    LOG_INFO("map-proto: GLOBAL stage-1 done in {:.1f} s ({}x{} grid)",
             secondsSince(s1Start), mapS1.sim.n, mapS1.sim.n);

    // Every tile finalizes against the ONE shared surface: the map
    // stage-1 answers for the center and all eight neighbours (its
    // window covers them), so the composite is the same ground on both
    // sides of every interior border by construction.
    const auto stage1At = [&](i32, i32) -> const TileStage1* {
        return &mapS1;
    };
    const i32 tx0 = mapTx * tilesPerSide;
    const i32 tz0 = mapTz * tilesPerSide;
    std::map<std::pair<i32, i32>, TileBakeResult> tiles;
    const auto s2Start = std::chrono::steady_clock::now();
    for (i32 dz = 0; dz < tilesPerSide; ++dz) {
        for (i32 dx = 0; dx < tilesPerSide; ++dx) {
            const auto tileStart = std::chrono::steady_clock::now();
            TileBakeResult tile =
                bakeTileStage2(params, tx0 + dx, tz0 + dz, stage1At);
            LOG_INFO("map-proto: tile ({}, {}) finalized in {:.1f} s — "
                     "{} lake(s), {} river(s)",
                     tx0 + dx, tz0 + dz, secondsSince(tileStart),
                     tile.lakes.size(), tile.rivers.size());
            tiles.emplace(std::make_pair(tx0 + dx, tz0 + dz),
                          std::move(tile));
        }
    }
    LOG_INFO("map-proto: {} tile(s) finalized in {:.1f} s total "
             "(+ {:.1f} s global stage-1)",
             tiles.size(), secondsSince(s2Start),
             secondsSince(s1Start) - secondsSince(s2Start));

    // Interior borders: the payoff measurement.
    const f32 margin = params.overlapMargin;
    f32 worstBand = 0.0f;
    for (i32 dz = 0; dz < tilesPerSide; ++dz) {
        for (i32 dx = 0; dx < tilesPerSide; ++dx) {
            const i32 tx = tx0 + dx;
            const i32 tz = tz0 + dz;
            const auto& a = tiles.at({ tx, tz });
            if (dx + 1 < tilesPerSide) {
                const auto& b = tiles.at({ tx + 1, tz });
                const f32 border = static_cast<f32>(tx + 1) * tileSize;
                const f32 alongMin = static_cast<f32>(tz) * tileSize;
                const f32 band = bandDivergence(
                    a.region, b.region, false, border, alongMin,
                    alongMin + tileSize, margin);
                worstBand = glm::max(worstBand, band);
                LOG_INFO("map-proto: border x={:.0f} — band divergence "
                         "max {:.3f} m",
                         border, band);
            }
            if (dz + 1 < tilesPerSide) {
                const auto& b = tiles.at({ tx, tz + 1 });
                const f32 border = static_cast<f32>(tz + 1) * tileSize;
                const f32 alongMin = static_cast<f32>(tx) * tileSize;
                const f32 band = bandDivergence(
                    a.region, b.region, true, border, alongMin,
                    alongMin + tileSize, margin);
                worstBand = glm::max(worstBand, band);
                LOG_INFO("map-proto: border z={:.0f} — band divergence "
                         "max {:.3f} m",
                         border, band);
            }
        }
    }
    LOG_INFO("map-proto: WORST interior band divergence {:.3f} m "
             "(windowed stage-1 baseline: 200-441 m — "
             "docs/CPU-PERF.md)",
             worstBand);
    return 0;
}

} // namespace cooker
