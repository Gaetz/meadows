#include "WaterBenchTool.hpp"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <glm/glm.hpp>

#include "WaterReplayTool.hpp"
#include "data/forms/LandscapeForms.hpp"
#include "data/plugins/PluginConfig.hpp"
#include "data/plugins/Resolver.hpp"
#include "engine/core/Log.hpp"
#include "engine/terrain/TerrainNoise.hpp"
#include "engine/terrain/WaterSim.hpp"
#include "engine/terrain/generation/MasterNetwork.hpp"
#include "engine/terrain/generation/TerrainGen.hpp"
#include "engine/terrain/generation/WaterSolve.hpp"
#include "game/AllForms.hpp"
#include "game/MapBaker.hpp"
#include "game/MapView.hpp"
#include "game/TerrainGenTuning.hpp"

namespace cooker {

namespace {

using render::terrain::WaterSimParams;
using render::terrain::WaterSimSnapshot;
using render::terrain::WaterSimState;
using render::terraingen::GridSpec;
using render::terraingen::TileBakeParams;

// What the PLAYER sees of the brooks: the published (hysteretic)
// snapshot over the brook cells, not the raw film.
void printPublishedBrooks(WaterSimState& state, const WaterSimParams& params,
                          const char* label) {
    if (state.brookMask.size() != state.spec.cells()) {
        return;
    }
    WaterSimSnapshot snap;
    render::terrain::extractSnapshot(state, params, snap);
    u32 cells = 0;
    u32 shown = 0;
    f64 sum = 0.0;
    for (size_t i = 0; i < state.spec.cells(); ++i) {
        if (state.brookMask[i] == 0) {
            continue;
        }
        ++cells;
        if (i < snap.depth.size() && snap.depth[i] > 0.0f) {
            ++shown;
            sum += snap.depth[i];
        }
    }
    if (cells > 0) {
        LOG_INFO("water-bench [{}]: brook cells {} | published {:.1f} % | "
                 "published mean depth {:.1f} mm",
                 label, cells,
                 100.0f * static_cast<f32>(shown) / static_cast<f32>(cells),
                 shown > 0 ? 1000.0 * sum / shown : 0.0);
    }
}

} // namespace

int waterBench(char** argv, int argc) {
    const std::filesystem::path gameDir = argv[2];
    const i32 mapX = std::atoi(argv[3]);
    const i32 mapZ = std::atoi(argv[4]);
    const f32 atX = static_cast<f32>(std::atof(argv[5]));
    const f32 atZ = static_cast<f32>(std::atof(argv[6]));
    u32 substeps = 3000; // 100 s of sim at dt 1/30
    f32 boost = -1.0f;   // < 0: the tuning form's value
    f32 pinHw = -1.0f;
    std::string probePath;
    std::string pngPrefix;
    for (int a = 7; a < argc; ++a) {
        const std::string arg = argv[a];
        if (arg == "--boost" && a + 1 < argc) {
            boost = static_cast<f32>(std::atof(argv[++a]));
        } else if (arg == "--pin" && a + 1 < argc) {
            pinHw = static_cast<f32>(std::atof(argv[++a]));
        } else if (arg == "--probe" && a + 1 < argc) {
            probePath = argv[++a];
        } else if (arg == "--png" && a + 1 < argc) {
            pngPrefix = argv[++a];
        } else {
            substeps = static_cast<u32>(std::atoi(argv[a]));
        }
    }

    // The game's data stack -> the game's bake params and sim tuning
    // (same resolution as landscape-report).
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
        LOG_ERROR("water-bench: no plugins under {} — wrong gameDir?",
                  dataDir.string());
        return 1;
    }
    data::FormDatabase forms;
    data::resolve(data::pointersOf(stack), formTypes, forms);
    const data::LandscapeTuningForm tuning =
        data::resolveLandscapeTuning(forms);
    const TileBakeParams params = game::makeTerrainBakeParams(
        tuning, data::resolveTerrainGenTuning(forms), &forms);
    const auto cacheDir = gameDir / "terrain-cache" /
                          std::to_string(tuning.terrainSeed);
    render::terraingen::setMasterNetworkCacheDir(cacheDir);
    std::optional<game::MapView> view = game::loadMapView(
        params, mapX, mapZ, game::kMapTilesPerSide, cacheDir);
    if (!view || view->slicesLoaded == 0 || !view->sandbox) {
        LOG_ERROR("water-bench: cannot read the baked map ({}, {}) under {} "
                  "— run landscape-report first",
                  mapX, mapZ, cacheDir.string());
        return 1;
    }

    // The sim's tuning, as the game resolves it from the form.
    WaterSimParams simParams;
    simParams.rainRate = tuning.waterSimRainRate;
    simParams.reservoirOutflow = tuning.waterSimReservoirOutflow;
    simParams.bakedDischargeBoost =
        boost >= 0.0f ? boost : tuning.waterSimBakedDischargeBoost;
    simParams.seaLevel = params.macro.seaLevel;
    if (pinHw < 0.0f) {
        pinHw = tuning.waterSimPinRiverHalfWidth;
    }
    const f32 texel = tuning.waterSimTexel;
    const f32 span = tuning.waterSimSpan;
    GridSpec spec;
    spec.texelSize = texel;
    spec.n = static_cast<u32>(span / texel) + 1;
    spec.originX = std::floor((atX - span * 0.5f) / texel) * texel;
    spec.originZ = std::floor((atZ - span * 0.5f) / texel) * texel;
    const f32 maxX =
        spec.originX + static_cast<f32>(spec.n - 1) * spec.texelSize;
    const f32 maxZ =
        spec.originZ + static_cast<f32>(spec.n - 1) * spec.texelSize;

    // Boundary inflow: the master network at the rect, then the baked
    // ribbons' entries, heads and distributed runoff (the game's
    // pre-roll recipe, WaterSimControl::simMaybeEnter).
    render::terraingen::ProceduralControlParams cp = params.controls;
    cp.seed = params.worldSeed;
    render::terraingen::MasterNetworkParams net = params.network;
    net.seaLevel = params.macro.seaLevel;
    net.mapGrid = view->sandbox->grid;
    render::terraingen::WaterSolveParams solveParams = params.waterSolve;
    solveParams.seaLevel = params.macro.seaLevel;
    vector<render::terraingen::WaterSource> sources =
        render::terraingen::masterBoundarySources(
            render::terraingen::ProceduralControls { cp }, params.macro,
            net, solveParams, spec.originX, spec.originZ, maxX, maxZ);
    const size_t masterCount = sources.size();
    const auto baked = render::terrain::entrySourcesFromBodies(
        view->bodies, spec, sources, simParams, pinHw);
    sources.insert(sources.end(), baked.begin(), baked.end());
    f64 masterQ = 0.0;
    f64 bakedQ = 0.0;
    for (size_t i = 0; i < sources.size(); ++i) {
        (i < masterCount ? masterQ : bakedQ) += sources[i].discharge;
    }
    LOG_INFO("water-bench: map ({}, {}) at ({:.0f}, {:.0f}) — window {}x{} "
             "at {} m, origin ({:.0f}, {:.0f}); pin >= {:.1f} m; boost "
             "x{:.2f}; sources: {} master ({:.1f} m3/s) + {} baked "
             "({:.1f} m3/s)",
             mapX, mapZ, atX, atZ, spec.n, spec.n, texel, spec.originX,
             spec.originZ, pinHw, simParams.bakedDischargeBoost,
             masterCount, masterQ, baked.size(), bakedQ);

    const render::TerrainParams& tp = view->tp;
    const auto heightFn = [&tp](f32 x, f32 z) {
        return render::terrain::height(tp, x, z);
    };
    WaterSimState state =
        render::terrain::preRollWindow(spec, heightFn, simParams, sources);
    render::terrain::pinLakes(state, view->bodies);
    render::terrain::pinRivers(state, view->bodies, pinHw);
    render::terrain::stepWindow(state, simParams, sources, 450);

    printStats("entry", stats(state, simParams));
    printPublishedBrooks(state, simParams, "entry");
    if (!pngPrefix.empty()) {
        renderMap(state, simParams, pngPrefix + "-entry.png");
    }
    if (!probePath.empty()) {
        printProbe(state, simParams, probePath, "entry");
    }
    if (substeps > 0) {
        render::terrain::stepWindow(state, simParams, sources, substeps);
        const std::string label =
            "t+" + std::to_string(substeps) + " substeps";
        printStats(label.c_str(), stats(state, simParams));
        printPublishedBrooks(state, simParams, label.c_str());
        if (!pngPrefix.empty()) {
            renderMap(state, simParams,
                      pngPrefix + "-t" + std::to_string(substeps) + ".png");
        }
        if (!probePath.empty()) {
            printProbe(state, simParams, probePath, label.c_str());
        }
    }
    return 0;
}

} // namespace cooker
