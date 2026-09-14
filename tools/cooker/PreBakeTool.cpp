#include "PreBakeTool.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>

#include "data/forms/LandscapeForms.hpp"
#include "data/plugins/PluginConfig.hpp"
#include "data/plugins/Resolver.hpp"
#include "engine/core/Jobs.hpp"
#include "engine/core/Log.hpp"
#include "game/AllForms.hpp"
#include "game/MapBaker.hpp"

namespace cooker {

// cooker pre-bake <gameDir> <x0> <z0> <x1> <z1>
// cooker pre-bake <gameDir> <centerX> <centerZ> <radiusMeters>
//
// <gameDir> is the game executable's directory: its data/ supplies the
// SAME plugin-resolved LandscapeTuningForm the game bakes with (seed,
// sea level, recurve — a pre-bake with different params would poison
// the cache), and its terrain-cache/<seed>/ receives the tiles.
int preBake(char** argv, int argc) {
    const std::filesystem::path gameDir = argv[2];
    f32 minX = 0.0f;
    f32 minZ = 0.0f;
    f32 maxX = 0.0f;
    f32 maxZ = 0.0f;
    if (argc == 7) {
        minX = static_cast<f32>(std::atof(argv[3]));
        minZ = static_cast<f32>(std::atof(argv[4]));
        maxX = static_cast<f32>(std::atof(argv[5]));
        maxZ = static_cast<f32>(std::atof(argv[6]));
    } else {
        const f32 cx = static_cast<f32>(std::atof(argv[3]));
        const f32 cz = static_cast<f32>(std::atof(argv[4]));
        const f32 r = static_cast<f32>(std::atof(argv[5]));
        minX = cx - r;
        maxX = cx + r;
        minZ = cz - r;
        maxZ = cz + r;
    }
    if (maxX < minX || maxZ < minZ) {
        LOG_ERROR("pre-bake: empty rect");
        return 1;
    }

    // The game's data stack -> the game's bake params (mirror of
    // LandscapeScene::bootstrapData + its sandbox block; language-pack
    // gating is irrelevant to terrain).
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
        LOG_ERROR("pre-bake: no plugins under {} — wrong gameDir?",
                  dataDir.string());
        return 1;
    }
    for (const str& error : stack.errors) {
        LOG_WARN("pre-bake: plugin stack: {}", error);
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

    const auto cacheDir = gameDir / "terrain-cache" /
                          std::to_string(tuning.terrainSeed);
    std::error_code ec;
    std::filesystem::create_directories(cacheDir, ec);

    // Bounded maps (chantier CARTES): the pre-bake unit is a whole map
    // — every map overlapping the rect, island rim (the game default),
    // skipped when its manifest already stands.
    const f32 mapSize =
        params.tileSize * static_cast<f32>(game::kMapTilesPerSide);
    const i32 mx0 = static_cast<i32>(std::floor(minX / mapSize));
    const i32 mx1 = static_cast<i32>(std::floor(maxX / mapSize));
    const i32 mz0 = static_cast<i32>(std::floor(minZ / mapSize));
    const i32 mz1 = static_cast<i32>(std::floor(maxZ / mapSize));
    LOG_INFO("pre-bake: seed {} sea {:.1f} | rect ({:.0f},{:.0f})-"
             "({:.0f},{:.0f}) = maps ({},{})..({},{}) -> {}",
             tuning.terrainSeed, tuning.seaLevel, minX, minZ, maxX, maxZ,
             mx0, mz0, mx1, mz1, cacheDir.string());

    core::JobSystem jobs; // one worker per hardware thread
    const auto start = std::chrono::steady_clock::now();
    u32 baked = 0;
    for (i32 mz = mz0; mz <= mz1; ++mz) {
        for (i32 mx = mx0; mx <= mx1; ++mx) {
            if (game::mapBakedAndValid(cacheDir, mx, mz,
                                       game::kMapTilesPerSide)) {
                LOG_INFO("pre-bake: map ({}, {}) already baked", mx,
                         mz);
                continue;
            }
            params.mapEdge = game::mapEdgeStylesFor(mx, mz);
            const game::MapBakeStats stats = game::bakeMap(
                params, mx, mz, cacheDir, &jobs,
                game::kMapTilesPerSide, [&](u32 landed, u32 total) {
                    LOG_INFO("pre-bake: map ({}, {}) [{}/{}] slice",
                             mx, mz, landed, total);
                });
            baked += stats.slicesWritten > 0 ? 1u : 0u;
        }
    }
    LOG_INFO("pre-bake: done — {} map(s) baked in {:.0f} s", baked,
             std::chrono::duration<f64> {
                 std::chrono::steady_clock::now() - start }
                 .count());
    return 0;
}

} // namespace cooker
