#include "LandscapeReport.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>

#include <stb_image_write.h>

#include "data/forms/LandscapeForms.hpp"
#include "data/plugins/PluginConfig.hpp"
#include "data/plugins/Resolver.hpp"
#include "engine/core/Jobs.hpp"
#include "engine/core/Log.hpp"
#include "engine/terrain/generation/MapExport.hpp"
#include "engine/terrain/generation/MasterNetwork.hpp"
#include "engine/terrain/generation/PoiPlan.hpp"
#include "engine/terrain/generation/TerrainGen.hpp"
#include "engine/terrain/generation/WorldLayer.hpp"
#include "game/AllForms.hpp"
#include "game/MapBaker.hpp"
#include "game/MapView.hpp"
#include "game/TerrainGenTuning.hpp"

namespace cooker {

namespace {

using render::terraingen::TileBakeParams;

struct Census {
    // Around the spawn (8 transects x 3 km, 4 m steps).
    f64 steep30 { 0.0 }; // % of steps > 30 deg
    f64 steep45 { 0.0 };
    f64 wallsPerKm { 0.0 }; // runs > 45 deg rising >= 5 m
    f32 wallMedian { 0.0f };
    f32 wallTallest { 0.0f };
    f32 relief250Spawn { 0.0f }; // median relief per 250 m window
    f32 rise100p95 { 0.0f };
    f64 meanSlopeSpawn { 0.0 }; // %
    // Map-wide (4 transects x 6 km, 10 m).
    f32 relief250Map { 0.0f };
    f64 highGroundSpawn { 0.0 }; // % of ground > 100 m above its 2 km min
    f64 highGroundMap { 0.0 };
    u32 lakes { 0 };
    u32 rivers { 0 };
};

f32 median(vector<f32>& v) {
    if (v.empty()) {
        return 0.0f;
    }
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

f32 percentile(vector<f32>& v, u32 pct) {
    if (v.empty()) {
        return 0.0f;
    }
    std::sort(v.begin(), v.end());
    return v[glm::min<size_t>(v.size() - 1, v.size() * pct / 100)];
}

// % of samples on a 100 m lattice (within `half` of the centre) that
// stand more than 100 m above the lowest overview point within 2 km.
f64 highGroundShare(const game::MapView& w, f32 cx, f32 cz, f32 half) {
    u32 samples = 0, high = 0;
    for (f32 z = cz - half; z <= cz + half; z += 100.0f) {
        for (f32 x = cx - half; x <= cx + half; x += 100.0f) {
            if (!w.inside(x, z, 400.0f)) {
                continue;
            }
            const f32 h = w.overviewHeight(x, z);
            if (h <= w.seaLevel() + 1.0f) {
                continue;
            }
            f32 lo = h;
            for (f32 dz = -2000.0f; dz <= 2000.0f; dz += 100.0f) {
                for (f32 dx = -2000.0f; dx <= 2000.0f; dx += 100.0f) {
                    if (dx * dx + dz * dz > 4.0e6f ||
                        !w.inside(x + dx, z + dz)) {
                        continue;
                    }
                    lo = glm::min(lo, w.overviewHeight(x + dx, z + dz));
                }
            }
            ++samples;
            high += (h - lo) > 100.0f;
        }
    }
    return samples ? 100.0 * high / samples : 0.0;
}

Census census(const game::MapView& w, const Vec3& spawn) {
    Census c;
    {
        constexpr f32 kStep = 4.0f;
        constexpr f32 kLen = 3000.0f;
        u64 steps = 0, s30 = 0, s45 = 0;
        f64 slopeSum = 0.0;
        f64 km = 0.0;
        vector<f32> walls, rises, reliefs;
        for (u32 t = 0; t < 8; ++t) {
            const f32 ang = static_cast<f32>(t) * 0.3926991f;
            const f32 dx = std::cos(ang);
            const f32 dz = std::sin(ang);
            const f32 x0 = spawn.x - dx * kLen * 0.5f;
            const f32 z0 = spawn.z - dz * kLen * 0.5f;
            if (!w.inside(x0, z0) ||
                !w.inside(x0 + dx * kLen, z0 + dz * kLen)) {
                continue;
            }
            f32 prev = w.height(x0, z0);
            f32 wallRise = 0.0f;
            vector<f32> last;
            f32 wLo = prev, wHi = prev;
            u32 inWindow = 0;
            for (f32 d = kStep; d <= kLen; d += kStep) {
                const f32 h = w.height(x0 + dx * d, z0 + dz * d);
                const f32 slope = std::abs(h - prev) / kStep;
                ++steps;
                km += kStep * 0.001;
                slopeSum += slope;
                s30 += slope > 0.5774f;
                s45 += slope > 1.0f;
                if (slope > 1.0f) {
                    wallRise += std::abs(h - prev);
                } else {
                    if (wallRise >= 5.0f) {
                        walls.push_back(wallRise);
                    }
                    wallRise = 0.0f;
                }
                last.push_back(h);
                if (last.size() > 25) {
                    rises.push_back(std::abs(h - last[last.size() - 26]));
                }
                wLo = glm::min(wLo, h);
                wHi = glm::max(wHi, h);
                if (++inWindow * kStep >= 250.0f) {
                    reliefs.push_back(wHi - wLo);
                    inWindow = 0;
                    wLo = wHi = h;
                }
                prev = h;
            }
            if (wallRise >= 5.0f) {
                walls.push_back(wallRise);
            }
        }
        const f64 n = static_cast<f64>(glm::max<u64>(steps, 1));
        c.steep30 = 100.0 * s30 / n;
        c.steep45 = 100.0 * s45 / n;
        c.meanSlopeSpawn = 100.0 * slopeSum / n;
        c.wallsPerKm = walls.size() / glm::max(km, 1e-3);
        c.wallMedian = median(walls);
        c.wallTallest = walls.empty() ? 0.0f : *std::max_element(walls.begin(), walls.end());
        c.rise100p95 = percentile(rises, 95);
        c.relief250Spawn = median(reliefs);
    }
    {
        constexpr f32 kStep = 10.0f;
        constexpr f32 kLen = 6000.0f;
        vector<f32> reliefs;
        for (u32 t = 0; t < 4; ++t) {
            const f32 ang = static_cast<f32>(t) * 0.7853982f + 0.3f;
            const f32 dx = std::cos(ang);
            const f32 dz = std::sin(ang);
            const f32 x0 = w.centreX() - dx * kLen * 0.5f;
            const f32 z0 = w.centreZ() - dz * kLen * 0.5f;
            f32 wLo = 1.0e9f, wHi = -1.0e9f;
            u32 inWindow = 0;
            for (f32 d = 0.0f; d <= kLen; d += kStep) {
                const f32 x = x0 + dx * d;
                const f32 z = z0 + dz * d;
                if (!w.inside(x, z, 400.0f)) {
                    continue;
                }
                const f32 h = w.height(x, z);
                if (h <= w.seaLevel() + 0.5f) {
                    continue;
                }
                wLo = glm::min(wLo, h);
                wHi = glm::max(wHi, h);
                if (++inWindow >= 25) {
                    reliefs.push_back(wHi - wLo);
                    inWindow = 0;
                    wLo = 1.0e9f;
                    wHi = -1.0e9f;
                }
            }
        }
        c.relief250Map = median(reliefs);
    }
    c.highGroundSpawn = highGroundShare(w, spawn.x, spawn.z, 1500.0f);
    c.highGroundMap =
        highGroundShare(w, w.centreX(), w.centreZ(), 0.5f * w.mapSize);
    c.lakes = static_cast<u32>(w.lakes.size());
    c.rivers = static_cast<u32>(w.rivers.size());
    return c;
}

} // namespace

int landscapeReport(char** argv, int argc) {
    const std::filesystem::path gameDir = argv[2];
    const i32 mapX = argc == 5 ? std::atoi(argv[3]) : 0;
    const i32 mapZ = argc == 5 ? std::atoi(argv[4]) : 0;

    // The game's data stack -> the game's bake params (pupitre included).
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
        LOG_ERROR("landscape-report: no plugins under {} — wrong gameDir?",
                  dataDir.string());
        return 1;
    }
    data::FormDatabase forms;
    data::resolve(data::pointersOf(stack), formTypes, forms);
    const data::LandscapeTuningForm tuning =
        data::resolveLandscapeTuning(forms);
    const TileBakeParams params = game::makeTerrainBakeParams(
        tuning, data::resolveTerrainGenTuning(forms));
    const auto cacheDir = gameDir / "terrain-cache" /
                          std::to_string(tuning.terrainSeed);
    std::error_code ec;
    std::filesystem::create_directories(cacheDir, ec);
    render::terraingen::setMasterNetworkCacheDir(cacheDir);

    // Cold costs of the stage-0 network and the plan's typing (what a
    // load pays in the spawn probe, what a bake pays in the imprint):
    // measured BEFORE the bake warms the process-wide memos.
    {
        render::terraingen::ProceduralControlParams cp = params.controls;
        cp.seed = params.worldSeed;
        const f32 mapSize =
            params.tileSize * static_cast<f32>(game::kMapTilesPerSide);
        const f32 cx = (static_cast<f32>(mapX) + 0.5f) * mapSize;
        const f32 cz = (static_cast<f32>(mapZ) + 0.5f) * mapSize;
        auto t0 = std::chrono::steady_clock::now();
        const auto lapMs = [&] {
            const auto now = std::chrono::steady_clock::now();
            const f64 ms =
                std::chrono::duration<f64, std::milli>(now - t0).count();
            t0 = now;
            return ms;
        };
        const auto sites = render::terraingen::poiSitesNear(
            cp.world, cp.poi, cx - 2500.0f, cz - 2500.0f, cx + 2500.0f,
            cz + 2500.0f);
        LOG_INFO("  cold: poiSitesNear (5 km rect, typing incl. the plan-free "
                 "network) {:.0f} ms, {} sites",
                 lapMs(), sites.size());
        const render::terraingen::ProceduralControls controls { cp };
        render::terraingen::MasterNetworkParams net = params.network;
        net.seaLevel = params.macro.seaLevel;
        const auto rivers = render::terraingen::masterRiversNear(
            controls, params.macro, net, cx - 0.5f * mapSize,
            cz - 0.5f * mapSize, cx + 0.5f * mapSize, cz + 0.5f * mapSize);
        LOG_INFO("  cold: masterRiversNear (plan on, the map rect) {:.0f} ms, "
                 "{} courses",
                 lapMs(), rivers.size());
        render::terraingen::masterRiversNear(
            controls, params.macro, net, cx - 0.5f * mapSize,
            cz - 0.5f * mapSize, cx + 0.5f * mapSize, cz + 0.5f * mapSize);
        LOG_INFO("  warm: masterRiversNear {:.1f} ms", lapMs());
    }
    const auto start = std::chrono::steady_clock::now();
    f64 bakeSeconds = 0.0;
    if (!game::mapBakedAndValid(cacheDir, mapX, mapZ, game::kMapTilesPerSide,
                                &params)) {
        core::JobSystem jobs;
        const game::MapBakeStats stats = game::bakeMap(
            params, mapX, mapZ, cacheDir, &jobs, game::kMapTilesPerSide,
            [&](u32 landed, u32 total) {
                LOG_INFO("landscape-report: map ({}, {}) [{}/{}] slice",
                         mapX, mapZ, landed, total);
            });
        bakeSeconds = std::chrono::duration<f64>(
                          std::chrono::steady_clock::now() - start)
                          .count();
        if (stats.slicesWritten == 0) {
            LOG_ERROR("landscape-report: bake failed");
            return 1;
        }
    }
    const auto view = game::loadMapView(params, mapX, mapZ,
                                        game::kMapTilesPerSide, cacheDir);
    if (!view || view->slicesLoaded == 0) {
        LOG_ERROR("landscape-report: cannot read the baked map");
        return 1;
    }

    // The plan render: controls + POI overlay, the map rect + a rim.
    {
        render::terraingen::ProceduralControlParams cp = params.controls;
        cp.seed = params.worldSeed;
        const render::terraingen::ProceduralControls controls { cp };
        render::terraingen::TerrainMapParams mp;
        mp.centerX = view->centreX();
        mp.centerZ = view->centreZ();
        mp.span = view->mapSize + 1000.0f;
        mp.size = 1024;
        mp.drawPoi = true;
        const vector<u8> pixels =
            render::terraingen::renderTerrainMap(controls, params.macro, mp);
        const auto png = view->mapDir / "plan.png";
        stbi_write_png(png.string().c_str(), 1024, 1024, 3, pixels.data(),
                       1024 * 3);
        LOG_INFO("landscape-report: plan -> {}", png.string());
    }

    // The analytic's cost per call (the far terrain, the far water, the
    // spawn probe and the control lattice all pay it).
    {
        render::terraingen::ProceduralControlParams cp = params.controls;
        cp.seed = params.worldSeed;
        const render::terraingen::ProceduralControls controls { cp };
        constexpr u32 kCalls = 20000;
        f32 sink = 0.0f;
        const auto timed = [&](const char* name, auto&& fn) {
            const auto t0 = std::chrono::steady_clock::now();
            for (u32 i = 0; i < kCalls; ++i) {
                const f32 x = view->minX + 400.0f +
                              static_cast<f32>((i * 7919u) % 7000u);
                const f32 z = view->minZ + 400.0f +
                              static_cast<f32>((i * 104729u) % 7000u);
                sink += fn(x, z);
            }
            const f64 us = std::chrono::duration<f64, std::micro>(
                               std::chrono::steady_clock::now() - t0)
                               .count() /
                           kCalls;
            LOG_INFO("  cost: {} {:.1f} us/call", name, us);
        };
        timed("worldSampleAt", [&](f32 x, f32 z) {
            return render::terraingen::worldSampleAt(cp.world, x, z).base;
        });
        timed("planSampleAt", [&](f32 x, f32 z) {
            return render::terraingen::planSampleAt(cp.world, cp.poi, x, z)
                .lift;
        });
        timed("controls.at", [&](f32 x, f32 z) {
            return controls.at(x, z).base;
        });
        timed("biomeIdAt", [&](f32 x, f32 z) {
            return static_cast<f32>(controls.biomeIdAt(x, z, 0.0f));
        });
        timed("macroHeightAnalytic", [&](f32 x, f32 z) {
            return render::terraingen::macroHeightAnalytic(controls,
                                                           params.macro, x, z);
        });
        if (sink == 12345.678f) {
            LOG_INFO("  (sink {})", sink);
        }
    }
    const Vec3 spawn = view->spawn();
    const Census c = census(*view, spawn);
    const u64 key = game::mapBakeKey(params, game::kMapTilesPerSide);
    LOG_INFO("landscape-report: map ({}, {}) seed {} key {:016x} bake {:.0f} s",
             mapX, mapZ, tuning.terrainSeed, key, bakeSeconds);
    LOG_INFO("  spawn ({:.0f}, {:.0f}, {:.0f})", spawn.x, spawn.y, spawn.z);
    LOG_INFO("  around the spawn: slope {:.1f} %, steps > 30 deg {:.1f} %, "
             "> 45 deg {:.1f} %, walls {:.1f}/km (median {:.0f} m, tallest "
             "{:.0f} m), relief {:.0f} m / 250 m, rise 100 m p95 {:.0f} m, "
             "ground > 100 m above its 2 km min {:.0f} %",
             c.meanSlopeSpawn, c.steep30, c.steep45, c.wallsPerKm,
             c.wallMedian, c.wallTallest, c.relief250Spawn, c.rise100p95,
             c.highGroundSpawn);
    LOG_INFO("  map: relief {:.0f} m / 250 m, ground > 100 m {:.0f} %, "
             "lakes {}, rivers {}",
             c.relief250Map, c.highGroundMap, c.lakes, c.rivers);
    // One line of history per run.
    std::ofstream log { cacheDir / "landscape-report.log", std::ios::app };
    if (log) {
        const std::time_t now = std::time(nullptr);
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M",
                      std::localtime(&now));
        log << stamp << " map " << mapX << "," << mapZ << " key " << std::hex
            << key << std::dec << " | spawn slope " << c.meanSlopeSpawn
            << " % >30 " << c.steep30 << " % >45 " << c.steep45
            << " % walls/km " << c.wallsPerKm << " relief250 "
            << c.relief250Spawn << " rise95 " << c.rise100p95 << " high "
            << c.highGroundSpawn << " % | map relief250 " << c.relief250Map
            << " high " << c.highGroundMap << " % lakes " << c.lakes
            << " rivers " << c.rivers << "\n";
    }
    return 0;
}

} // namespace cooker
