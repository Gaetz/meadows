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
#include "game/TerrainBakeStreamer.hpp"

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

    const f32 t = params.tileSize;
    const i32 tilesX = static_cast<i32>(std::floor(maxX / t)) -
                       static_cast<i32>(std::floor(minX / t)) + 1;
    const i32 tilesZ = static_cast<i32>(std::floor(maxZ / t)) -
                       static_cast<i32>(std::floor(minZ / t)) + 1;
    LOG_INFO("pre-bake: seed {} sea {:.1f} | rect ({:.0f},{:.0f})-"
             "({:.0f},{:.0f}) = {}x{} tiles (v{}) -> {}",
             tuning.terrainSeed, tuning.seaLevel, minX, minZ, maxX, maxZ,
             tilesX, tilesZ, render::terraingen::kTileBakeVersion,
             cacheDir.string());

    core::JobSystem jobs; // one worker per hardware thread
    game::TerrainBakeStreamer streamer { params, cacheDir, &jobs };
    streamer.requestRect(minX, minZ, maxX, maxZ);
    const u32 total = streamer.pendingCount();
    LOG_INFO("pre-bake: {} tile(s) requested (cache hits publish "
             "instantly)",
             total);

    const auto start = std::chrono::steady_clock::now();
    u32 done = 0;
    while (streamer.pendingCount() > 0) {
        streamer.drain([&](game::TerrainBakeStreamer::PublishedTile&&
                               tile) {
            ++done;
            LOG_INFO("pre-bake: [{}/{}] tile ({}, {}) — {} lake(s), "
                     "{} river(s) | stage-1 {} | {:.0f} s elapsed",
                     done, total, tile.tx, tile.tz, tile.lakes.size(),
                     tile.rivers.size(), streamer.stage1Count(),
                     std::chrono::duration<f64> {
                         std::chrono::steady_clock::now() - start }
                         .count());
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    streamer.drain([&](game::TerrainBakeStreamer::PublishedTile&&) {
        ++done;
    });
    LOG_INFO("pre-bake: done — {} tile(s) in {:.0f} s", done,
             std::chrono::duration<f64> {
                 std::chrono::steady_clock::now() - start }
                 .count());
    return 0;
}

} // namespace cooker
