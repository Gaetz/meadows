#include "BorderReportTool.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <thread>

#include "data/forms/LandscapeForms.hpp"
#include "data/plugins/PluginConfig.hpp"
#include "data/plugins/Resolver.hpp"
#include "engine/core/Jobs.hpp"
#include "engine/core/Log.hpp"
#include "engine/render/landscape/TerrainNoise.hpp"
#include "engine/terrain/SandboxTerrain.hpp"
#include "engine/terrain/TerrainBase.hpp"
#include "game/AllForms.hpp"
#include "game/TerrainBakeStreamer.hpp"

namespace cooker {

namespace {

using game::TerrainBakeStreamer;
using render::terraingen::Lake;

// Fraction of the mask's shoreline (wet cells with a dry/out-of-mask
// 4-neighbour) sitting within one texel of the given rect's edge — the
// axis-cut detector: a lake sliced by the owner rect scores near the
// clipped side's share of its perimeter, a natural shoreline near 0.
f32 rectEdgeTruncation(const Lake& lake, f32 rectMinX, f32 rectMinZ,
                       f32 rectMaxX, f32 rectMaxZ) {
    if (lake.mask.empty() || lake.maskWidth == 0) {
        return 0.0f;
    }
    u32 perimeter = 0;
    u32 onEdge = 0;
    const auto wet = [&](i32 c, i32 r) {
        if (c < 0 || r < 0 || c >= static_cast<i32>(lake.maskWidth) ||
            r >= static_cast<i32>(lake.maskHeight)) {
            return false;
        }
        return lake.mask[static_cast<size_t>(r) * lake.maskWidth + c] !=
               0;
    };
    for (u32 r = 0; r < lake.maskHeight; ++r) {
        for (u32 c = 0; c < lake.maskWidth; ++c) {
            if (!wet(static_cast<i32>(c), static_cast<i32>(r))) {
                continue;
            }
            const bool shore = !wet(static_cast<i32>(c) - 1,
                                    static_cast<i32>(r)) ||
                               !wet(static_cast<i32>(c) + 1,
                                    static_cast<i32>(r)) ||
                               !wet(static_cast<i32>(c),
                                    static_cast<i32>(r) - 1) ||
                               !wet(static_cast<i32>(c),
                                    static_cast<i32>(r) + 1);
            if (!shore) {
                continue;
            }
            ++perimeter;
            const f32 wx =
                lake.minX + static_cast<f32>(c) * lake.maskTexel;
            const f32 wz =
                lake.minZ + static_cast<f32>(r) * lake.maskTexel;
            const f32 edge =
                glm::min(glm::min(wx - rectMinX, rectMaxX - wx),
                         glm::min(wz - rectMinZ, rectMaxZ - wz));
            if (edge <= lake.maskTexel) {
                ++onEdge;
            }
        }
    }
    return perimeter > 0
               ? static_cast<f32>(onEdge) / static_cast<f32>(perimeter)
               : 0.0f;
}

// Mirror of the runtime dedupe sampling (LandscapeScene
// publishBakedTiles): every 3rd wet cell of the smaller mask tested
// against the bigger's nearest texel.
f32 sharedFraction(const Lake& la, const Lake& lb) {
    const auto covers = [](const Lake& lake, f32 x, f32 z) {
        if (x < lake.minX || x > lake.maxX || z < lake.minZ ||
            z > lake.maxZ || lake.mask.empty()) {
            return false;
        }
        const u32 mx = static_cast<u32>(glm::clamp(
            (x - lake.minX) / lake.maskTexel + 0.5f, 0.0f,
            static_cast<f32>(lake.maskWidth - 1)));
        const u32 mz = static_cast<u32>(glm::clamp(
            (z - lake.minZ) / lake.maskTexel + 0.5f, 0.0f,
            static_cast<f32>(lake.maskHeight - 1)));
        return lake.mask[static_cast<size_t>(mz) * lake.maskWidth +
                         mx] != 0;
    };
    const bool aSmall = la.cells <= lb.cells;
    const Lake& small = aSmall ? la : lb;
    const Lake& big = aSmall ? lb : la;
    u32 sampled = 0;
    u32 shared = 0;
    for (u32 mz = 0; mz < small.maskHeight; mz += 3) {
        for (u32 mx = 0; mx < small.maskWidth; mx += 3) {
            if (!small.mask[static_cast<size_t>(mz) * small.maskWidth +
                            mx]) {
                continue;
            }
            ++sampled;
            const f32 x =
                small.minX + static_cast<f32>(mx) * small.maskTexel;
            const f32 z =
                small.minZ + static_cast<f32>(mz) * small.maskTexel;
            if (covers(big, x, z)) {
                ++shared;
            }
        }
    }
    return sampled > 0
               ? static_cast<f32>(shared) / static_cast<f32>(sampled)
               : 0.0f;
}

} // namespace

int borderReport(char** argv, int argc) {
    const std::filesystem::path gameDir = argv[2];
    const i32 tx = std::atoi(argv[3]);
    const i32 tz = std::atoi(argv[4]);
    const bool axisZ = argc >= 6 && std::strcmp(argv[5], "z") == 0;
    const char* csvPath = argc >= 7 ? argv[6] : nullptr;
    const i32 ntx = tx + (axisZ ? 0 : 1);
    const i32 ntz = tz + (axisZ ? 1 : 0);

    // The game's data stack -> the game's bake params (same resolution
    // as pre-bake — a report on different params would be meaningless).
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
        LOG_ERROR("border-report: no plugins under {} — wrong gameDir?",
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

    const auto cacheDir = gameDir / "terrain-cache" /
                          std::to_string(tuning.terrainSeed);
    std::error_code ec;
    std::filesystem::create_directories(cacheDir, ec);

    const f32 t = params.tileSize;
    LOG_INFO("border-report: seed {} | tiles ({},{}) vs ({},{}) — "
             "border {} = {:.0f} (v{})",
             tuning.terrainSeed, tx, tz, ntx, ntz, axisZ ? "z" : "x",
             axisZ ? static_cast<f32>(ntz) * t
                   : static_cast<f32>(ntx) * t,
             render::terraingen::kTileBakeVersion);

    core::JobSystem jobs;
    TerrainBakeStreamer streamer { params, cacheDir, &jobs };
    const f32 cxA = (static_cast<f32>(tx) + 0.5f) * t;
    const f32 czA = (static_cast<f32>(tz) + 0.5f) * t;
    const f32 cxB = (static_cast<f32>(ntx) + 0.5f) * t;
    const f32 czB = (static_cast<f32>(ntz) + 0.5f) * t;
    streamer.requestRect(glm::min(cxA, cxB), glm::min(czA, czB),
                         glm::max(cxA, cxB), glm::max(czA, czB));

    std::map<std::pair<i32, i32>, TerrainBakeStreamer::PublishedTile>
        tiles;
    while (streamer.pendingCount() > 0) {
        streamer.drain(
            [&](TerrainBakeStreamer::PublishedTile&& tile) {
                LOG_INFO("border-report: tile ({}, {}) ready — {} "
                         "lake(s)",
                         tile.tx, tile.tz, tile.lakes.size());
                tiles.emplace(std::make_pair(tile.tx, tile.tz),
                              std::move(tile));
            });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    streamer.drain([&](TerrainBakeStreamer::PublishedTile&& tile) {
        tiles.emplace(std::make_pair(tile.tx, tile.tz),
                      std::move(tile));
    });
    const auto itA = tiles.find({ tx, tz });
    const auto itB = tiles.find({ ntx, ntz });
    if (itA == tiles.end() || itB == tiles.end()) {
        LOG_ERROR("border-report: bake did not deliver both tiles");
        return 1;
    }
    const auto& tileA = itA->second;
    const auto& tileB = itB->second;
    const render::TerrainRegion& ra = tileA.region;
    const render::TerrainRegion& rb = tileB.region;

    // --- 1. Band divergence profile -------------------------------
    // Both rects cover border ± overlapMargin; sample the whole shared
    // band. Along-border coordinate spans the common tile edge.
    const f32 border =
        axisZ ? static_cast<f32>(ntz) * t : static_cast<f32>(ntx) * t;
    const f32 margin = params.overlapMargin;
    const f32 alongMin = axisZ ? static_cast<f32>(tx) * t
                               : static_cast<f32>(tz) * t;
    const f32 alongMax = alongMin + t;
    std::ofstream csv;
    if (csvPath) {
        csv.open(csvPath, std::ios::trunc);
        csv << "offset_m,mean_delta_m,max_delta_m\n";
    }
    f32 bandMax = 0.0f;
    f32 bandMaxAlong = 0.0f;
    for (f32 d = -margin + 2.0f; d <= margin - 2.0f; d += 2.0f) {
        f64 sum = 0.0;
        u32 n = 0;
        f32 rowMax = 0.0f;
        for (f32 s = alongMin + 2.0f; s < alongMax; s += 4.0f) {
            const f32 x = axisZ ? s : border + d;
            const f32 z = axisZ ? border + d : s;
            const f32 ha = render::terrain::baseHeight(ra, x, z);
            const f32 hb = render::terrain::baseHeight(rb, x, z);
            const f32 delta = std::abs(ha - hb);
            sum += delta;
            ++n;
            if (delta > rowMax) {
                rowMax = delta;
            }
            if (delta > bandMax) {
                bandMax = delta;
                bandMaxAlong = s;
            }
        }
        if (csv.is_open()) {
            csv << d << ',' << (n ? sum / n : 0.0) << ',' << rowMax
                << '\n';
        }
    }
    const f32 blendSpan = 2.0f * margin;
    LOG_INFO("border-report: band divergence max {:.2f} m (at "
             "along={:.0f}) — implied blend slope {:.1f} deg over "
             "{:.0f} m",
             bandMax, bandMaxAlong,
             glm::degrees(std::atan(1.5f * bandMax / blendSpan)),
             blendSpan);

    // --- Blend walk: the surface the player actually sees ---------
    {
        auto base = std::make_shared<render::TerrainBase>();
        base->regions.push_back(
            std::make_shared<render::TerrainRegion>(ra));
        base->regions.push_back(
            std::make_shared<render::TerrainRegion>(rb));
        base->buildIndex();
        render::TerrainParams tp;
        tp.base = base;
        auto sandbox = std::make_shared<render::SandboxTerrain>();
        sandbox->controls = params.controls;
        sandbox->controls.seed = params.worldSeed;
        sandbox->macro = params.macro;
        tp.sandbox = sandbox;
        f32 walkMax = 0.0f;
        f32 walkMaxAlong = 0.0f;
        for (f32 s = alongMin + 128.0f; s < alongMax; s += 256.0f) {
            f32 previous = render::terrain::height(
                tp, axisZ ? s : border - 80.0f,
                axisZ ? border - 80.0f : s);
            for (f32 d = -79.0f; d <= 80.0f; d += 1.0f) {
                const f32 h = render::terrain::height(
                    tp, axisZ ? s : border + d,
                    axisZ ? border + d : s);
                const f32 step = std::abs(h - previous);
                if (step > walkMax) {
                    walkMax = step;
                    walkMaxAlong = s;
                }
                previous = h;
            }
        }
        LOG_INFO("border-report: blended walk max step {:.2f} m/m "
                 "({:.1f} deg, at along={:.0f})",
                 walkMax, glm::degrees(std::atan(walkMax)),
                 walkMaxAlong);
    }

    // --- 2. Lake stats --------------------------------------------
    const auto lakeReport = [&](const char* tag,
                                const TerrainBakeStreamer::
                                    PublishedTile& tile,
                                const render::TerrainRegion& region) {
        const f32 rMinX = region.originX;
        const f32 rMinZ = region.originZ;
        const f32 rMaxX = region.originX + region.spanX();
        const f32 rMaxZ = region.originZ + region.spanZ();
        for (const Lake& lake : tile.lakes) {
            const f32 trunc = rectEdgeTruncation(lake, rMinX, rMinZ,
                                                 rMaxX, rMaxZ);
            if (trunc < 0.05f && lake.cells < 50) {
                continue; // small natural ponds are noise here
            }
            LOG_INFO("border-report: [{}] lake level {:.2f} cells {} "
                     "bbox ({:.0f},{:.0f})-({:.0f},{:.0f}) — rect-edge "
                     "truncation {:.0f} %",
                     tag, lake.level, lake.cells, lake.minX, lake.minZ,
                     lake.maxX, lake.maxZ, trunc * 100.0f);
        }
    };
    lakeReport("A", tileA, ra);
    lakeReport("B", tileB, rb);

    // Cross-tile duplicate views (the runtime dedupe's prey).
    for (const Lake& la : tileA.lakes) {
        for (const Lake& lb : tileB.lakes) {
            if (la.minX > lb.maxX || lb.minX > la.maxX ||
                la.minZ > lb.maxZ || lb.minZ > la.maxZ) {
                continue;
            }
            const f32 shared = sharedFraction(la, lb);
            if (shared < 0.05f) {
                continue;
            }
            LOG_INFO("border-report: cross-tile pair — A level {:.2f} "
                     "({} cells) vs B level {:.2f} ({} cells), shared "
                     "{:.0f} % (dedupe fires at 30 %), level gap "
                     "{:.2f} m",
                     la.level, la.cells, lb.level, lb.cells,
                     shared * 100.0f, std::abs(la.level - lb.level));
        }
    }

    // --- 3. Bed-carve asymmetry -----------------------------------
    // For each lake reaching the shared band: at its deepest in-band
    // wet cell (owner's ground), compare the two regions' ground.
    const auto carveReport = [&](const char* tag,
                                 const TerrainBakeStreamer::
                                     PublishedTile& tile,
                                 const render::TerrainRegion& own,
                                 const render::TerrainRegion& other) {
        for (const Lake& lake : tile.lakes) {
            f32 deepest = 1.0e9f;
            f32 bx = 0.0f;
            f32 bz = 0.0f;
            bool inBand = false;
            for (u32 r = 0; r < lake.maskHeight; ++r) {
                for (u32 c = 0; c < lake.maskWidth; ++c) {
                    if (!lake.mask[static_cast<size_t>(r) *
                                       lake.maskWidth +
                                   c]) {
                        continue;
                    }
                    const f32 wx =
                        lake.minX +
                        static_cast<f32>(c) * lake.maskTexel;
                    const f32 wz =
                        lake.minZ +
                        static_cast<f32>(r) * lake.maskTexel;
                    const f32 d = axisZ ? wz - border : wx - border;
                    if (std::abs(d) > margin - 2.0f ||
                        !own.contains(wx, wz) ||
                        !other.contains(wx, wz)) {
                        continue;
                    }
                    inBand = true;
                    const f32 g =
                        render::terrain::baseHeight(own, wx, wz);
                    if (g < deepest) {
                        deepest = g;
                        bx = wx;
                        bz = wz;
                    }
                }
            }
            if (!inBand) {
                continue;
            }
            const f32 gOwn = render::terrain::baseHeight(own, bx, bz);
            const f32 gOther =
                render::terrain::baseHeight(other, bx, bz);
            LOG_INFO("border-report: [{}] lake level {:.2f} in-band "
                     "deepest ({:.0f},{:.0f}) — own depth {:.2f} m, "
                     "other depth {:.2f} m, carve asymmetry {:.2f} m",
                     tag, lake.level, bx, bz, lake.level - gOwn,
                     lake.level - gOther, std::abs(gOwn - gOther));
        }
    };
    carveReport("A", tileA, ra, rb);
    carveReport("B", tileB, rb, ra);

    if (csvPath) {
        LOG_INFO("border-report: profile written to {}", csvPath);
    }
    return 0;
}

} // namespace cooker
