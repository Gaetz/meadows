#include "LandscapeReport.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
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
#include "engine/terrain/generation/ZonePlan.hpp"
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
        tuning, data::resolveTerrainGenTuning(forms), &forms);
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
        mp.drawGates = true;
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
        // The SWEEP: 40 km at 200 m, every sample in fresh country (the
        // headless tests' pattern: memo misses dominate).
        const auto sweep = [&](const char* name, auto&& fn) {
            const auto t0 = std::chrono::steady_clock::now();
            u32 calls = 0;
            for (f32 z = -40000.0f; z <= 40000.0f; z += 200.0f) {
                for (f32 x = -40000.0f; x <= 40000.0f; x += 200.0f) {
                    sink += fn(x, z);
                    ++calls;
                }
            }
            const f64 us = std::chrono::duration<f64, std::micro>(
                               std::chrono::steady_clock::now() - t0)
                               .count() /
                           glm::max(calls, 1u);
            LOG_INFO("  sweep 80 km @200 m: {} {:.1f} us/call", name, us);
        };
        sweep("worldSampleAt", [&](f32 x, f32 z) {
            return render::terraingen::worldSampleAt(cp.world, x, z).base;
        });
        sweep("zoneSampleAt", [&](f32 x, f32 z) {
            return render::terraingen::zoneSampleAt(cp.world, cp.zones, x, z)
                .storeyHeight;
        });
        sweep("planSampleAt", [&](f32 x, f32 z) {
            return render::terraingen::planSampleAt(cp.world, cp.poi, x, z)
                .lift;
        });
        sweep("controls.at", [&](f32 x, f32 z) {
            return controls.at(x, z).base;
        });
        // The headless tests' params: the C++ defaults, no archetype
        // records (the default table), no network cache dir.
        render::terraingen::ProceduralControlParams defaults;
        defaults.seed = 1337;
        const render::terraingen::ProceduralControls defControls { defaults };
        sweep("controls.at (C++ defaults)", [&](f32 x, f32 z) {
            return defControls.at(x, z).base;
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
    {
        // Where the lakes sit against the zones: on a wall's foot or
        // rim (a hollow the storey step traps), on a piece pond, or
        // elsewhere (the hydrology's own).
        render::terraingen::ProceduralControlParams cp = params.controls;
        cp.seed = params.worldSeed;
        u32 nearWall = 0, onPond = 0, other = 0;
        for (const auto& lake : view->lakes) {
            const f32 lx = 0.5f * (lake.minX + lake.maxX);
            const f32 lz = 0.5f * (lake.minZ + lake.maxZ);
            const render::terraingen::ZoneSample zs =
                render::terraingen::zoneSampleAt(cp.world, cp.zones, lx, lz);
            if (zs.wallSteps > 0.0f && std::abs(zs.borderDist) < 200.0f) {
                ++nearWall;
            } else if (zs.pieceBasin > 0.5f) {
                ++onPond;
            } else {
                ++other;
            }
        }
        LOG_INFO("  lakes vs zones: within 200 m of a wall {}, on a piece pond "
                 "{}, elsewhere {}",
                 nearWall, onPond, other);
        // Walls and gates (Z3): a 16 m raster of the map; gate blobs
        // and wall stretches between them (4-connected components),
        // the longest stretch without a passage (August's rule: none
        // over 400 m).
        {
            const f32 step = 16.0f;
            const u32 n = static_cast<u32>(view->mapSize / step) + 1;
            vector<u8> kind(static_cast<size_t>(n) * n, 0); // 1 wall, 2 gate, 3 ridge
            vector<u8> gateKind(kind.size(), 0);
            u32 notch = 0, breach = 0, col = 0;
            for (u32 row = 0; row < n; ++row) {
                for (u32 c = 0; c < n; ++c) {
                    const render::terraingen::ZoneSample zs =
                        render::terraingen::zoneSampleAt(
                            cp.world, cp.zones, view->minX + c * step,
                            view->minZ + row * step);
                    u8 k = 0;
                    if (zs.gate > 0.5f) {
                        k = 2;
                        gateKind[static_cast<size_t>(row) * n + c] = zs.gateKind;
                    } else if (zs.wall > 0.5f) {
                        k = 1;
                    } else if (zs.ridge > 0.5f * cp.zones.ridgeHeight) {
                        k = 3;
                    }
                    kind[static_cast<size_t>(row) * n + c] = k;
                }
            }
            // Distance from every wall texel to the nearest gate (a 3-4
            // chamfer transform): how far one walks along a wall before
            // a passage. The honest form of "no wall over 400 m".
            vector<u32> dt(kind.size(), 1u << 20);
            for (u32 i = 0; i < kind.size(); ++i) {
                if (kind[i] == 2) {
                    dt[i] = 0;
                }
            }
            const auto relax = [&](u32 i, i32 dr, i32 dc, u32 cost) {
                const i32 r = static_cast<i32>(i / n) + dr;
                const i32 c = static_cast<i32>(i % n) + dc;
                if (r < 0 || c < 0 || r >= static_cast<i32>(n) || c >= static_cast<i32>(n)) {
                    return;
                }
                const u32 j = static_cast<u32>(r) * n + static_cast<u32>(c);
                dt[i] = glm::min(dt[i], dt[j] + cost);
            };
            for (u32 i = 0; i < kind.size(); ++i) {
                relax(i, -1, -1, 4); relax(i, -1, 0, 3); relax(i, -1, 1, 4); relax(i, 0, -1, 3);
            }
            for (u32 i = static_cast<u32>(kind.size()); i-- > 0;) {
                relax(i, 1, 1, 4); relax(i, 1, 0, 3); relax(i, 1, -1, 4); relax(i, 0, 1, 3);
            }
            vector<f32> wallToGate;
            for (u32 i = 0; i < kind.size(); ++i) {
                if (kind[i] == 1 && dt[i] < (1u << 20)) {
                    wallToGate.push_back(static_cast<f32>(dt[i]) / 3.0f * step);
                }
            }
            std::sort(wallToGate.begin(), wallToGate.end());
            const f32 farthest = wallToGate.empty() ? 0.0f : wallToGate.back();
            const f32 far95 = wallToGate.empty() ? 0.0f : wallToGate[wallToGate.size() * 95 / 100];
            vector<u32> label(kind.size(), 0);
            u32 gates = 0, stretches = 0;
            vector<f32> extents;
            vector<u32> stack;
            for (u32 i = 0; i < kind.size(); ++i) {
                if (kind[i] == 0 || kind[i] == 3 || label[i]) {
                    continue;
                }
                const u8 k = kind[i];
                u32 minC = n, maxC = 0, minR = n, maxR = 0;
                stack.assign(1, i);
                label[i] = 1;
                while (!stack.empty()) {
                    const u32 j = stack.back();
                    stack.pop_back();
                    const u32 r = j / n, c = j % n;
                    minC = glm::min(minC, c); maxC = glm::max(maxC, c);
                    minR = glm::min(minR, r); maxR = glm::max(maxR, r);
                    const i32 dr[4] = { 0, 0, -1, 1 };
                    const i32 dc[4] = { -1, 1, 0, 0 };
                    for (u32 d = 0; d < 4; ++d) {
                        const i32 rr = static_cast<i32>(r) + dr[d];
                        const i32 cc = static_cast<i32>(c) + dc[d];
                        if (rr < 0 || cc < 0 || rr >= static_cast<i32>(n) ||
                            cc >= static_cast<i32>(n)) {
                            continue;
                        }
                        const u32 jj = static_cast<u32>(rr) * n + static_cast<u32>(cc);
                        if (kind[jj] == k && !label[jj]) {
                            label[jj] = 1;
                            stack.push_back(jj);
                        }
                    }
                }
                if (k == 2) {
                    ++gates;
                    notch += gateKind[i] == 1;
                    breach += gateKind[i] == 2;
                    col += gateKind[i] == 3;
                } else {
                    ++stretches;
                    extents.push_back(static_cast<f32>(glm::max(maxC - minC, maxR - minR)) * step);
                }
            }
            std::sort(extents.begin(), extents.end());
            const f32 longest = extents.empty() ? 0.0f : extents.back();
            const f32 p95 = extents.empty() ? 0.0f : extents[extents.size() * 95 / 100];
            u32 over400 = 0;
            for (const f32 e : extents) {
                over400 += e > 400.0f;
            }
            LOG_INFO("  walls and gates: {} gates ({} notches, {} breaches, {} cols), "
                     "{} wall stretches, longest {:.0f} m, p95 {:.0f} m, {} over 400 m; "
                     "wall point to its nearest gate p95 {:.0f} m, farthest {:.0f} m",
                     gates, notch, breach, col, stretches, longest, p95, over400,
                     far95, farthest);
        }
        // Rivers hugging a map line (dev bug report 2026-10-08): nodes
        // within 150 m of the map's four border lines, per tier, and
        // the longest run of consecutive nodes that stays there.
        u32 nodesTotal = 0, nodesNear = 0;
        u32 longestRun = 0;
        u8 longestTier = 0;
        f32 longestX = 0.0f, longestZ = 0.0f;
        const auto lineDist = [&](f32 x, f32 z) {
            return glm::min(
                glm::min(std::abs(x - view->minX), std::abs(x - view->maxX)),
                glm::min(std::abs(z - view->minZ), std::abs(z - view->maxZ)));
        };
        // A run's EXTENT along the line (meters between its first and
        // last node, projected on the line) tells a hug from an oblique
        // crossing: a 25-degree crossing stays within 150 m over 650 m
        // without ever running along.
        f32 longestAlong = 0.0f, longestAcross = 0.0f;
        for (const auto& river : view->rivers) {
            u32 run = 0;
            f32 runX0 = 0.0f, runZ0 = 0.0f;
            for (const auto& node : river.points) {
                ++nodesTotal;
                if (lineDist(node.x, node.z) < 150.0f) {
                    ++nodesNear;
                    if (run == 0) {
                        runX0 = node.x;
                        runZ0 = node.z;
                    }
                    ++run;
                    // Which line: the nearer axis decides along/across.
                    const f32 dx = glm::min(std::abs(node.x - view->minX),
                                            std::abs(node.x - view->maxX));
                    const f32 dz = glm::min(std::abs(node.z - view->minZ),
                                            std::abs(node.z - view->maxZ));
                    const bool vertical = dx < dz;
                    const f32 along = vertical ? std::abs(node.z - runZ0)
                                               : std::abs(node.x - runX0);
                    // The worst run is the one that goes FURTHEST along
                    // the line, whatever its node count.
                    if (along > longestAlong) {
                        longestAlong = along;
                        longestAcross = vertical ? std::abs(node.x - runX0)
                                                 : std::abs(node.z - runZ0);
                        longestRun = run;
                        longestTier = river.tier;
                        longestX = node.x;
                        longestZ = node.z;
                    }
                } else {
                    run = 0;
                }
            }
        }
        LOG_INFO("  rivers vs map lines: {} of {} nodes within 150 m of a line, "
                 "longest run {} nodes, tier {} (last at {:.0f}, {:.0f}; ground "
                 "there {:.1f} m, sea level {:.1f}); it spans {:.0f} m along "
                 "the line for {:.0f} m across",
                 nodesNear, nodesTotal, longestRun, longestTier, longestX,
                 longestZ, view->height(longestX, longestZ), view->seaLevel(),
                 longestAlong, longestAcross);
        // The MASTER network's own courses (analytic, 128 m, no map
        // border shape): if a master course hugs the line, the imprint
        // carves it there whatever the bake does afterwards.
        {
            render::terraingen::MasterNetworkParams network = params.network;
            network.seaLevel = params.macro.seaLevel;
            const auto master = render::terraingen::masterRiversNear(
                render::terraingen::ProceduralControls { cp }, params.macro,
                network, view->minX - 1024.0f, view->minZ - 1024.0f,
                view->maxX + 1024.0f, view->maxZ + 1024.0f);
            u32 mTotal = 0, mNear = 0, mRun = 0;
            f32 mX = 0.0f, mZ = 0.0f;
            for (const auto& river : master) {
                u32 run = 0;
                for (const auto& node : river.nodes) {
                    ++mTotal;
                    if (lineDist(node.x, node.z) < 150.0f) {
                        ++mNear;
                        if (++run > mRun) {
                            mRun = run;
                            mX = node.x;
                            mZ = node.z;
                        }
                    } else {
                        run = 0;
                    }
                }
            }
            LOG_INFO("  master courses vs map lines: {} courses, {} of {} nodes "
                     "within 150 m of a line, longest run {} nodes (last at "
                     "{:.0f}, {:.0f})",
                     master.size(), mNear, mTotal, mRun, mX, mZ);
            // Each fleuve course touching the map: where it rises, where
            // it ends, whether the routing reached the sea or the
            // super-cell rim (the dev's question: a fleuve walled in
            // mid-map — does it come from anywhere, go anywhere?).
            for (const auto& river : master) {
                if (river.nodes.empty()) {
                    continue;
                }
                const auto& first = river.nodes.front();
                const auto& lastNode = river.nodes.back();
                f32 length = 0.0f;
                for (size_t k = 1; k < river.nodes.size(); ++k) {
                    length += std::hypot(river.nodes[k].x - river.nodes[k - 1].x,
                                         river.nodes[k].z - river.nodes[k - 1].z);
                }
                // A course that stops on another course's node is a
                // tributary (the tracer breaks at the confluence).
                bool joins = false;
                for (const auto& other : master) {
                    if (&other == &river) {
                        continue;
                    }
                    for (const auto& node : other.nodes) {
                        if (node.x == lastNode.x && node.z == lastNode.z) {
                            joins = true;
                            break;
                        }
                    }
                    if (joins) {
                        break;
                    }
                }
                LOG_INFO("    fleuve: {} nodes, {:.1f} km, from ({:.0f}, {:.0f}) "
                         "surface {:.0f} m to ({:.0f}, {:.0f}) surface {:.0f} m, "
                         "area {:.0f} km2, {}",
                         river.nodes.size(), length / 1000.0f, first.x, first.z,
                         first.surface, lastNode.x, lastNode.z, lastNode.surface,
                         lastNode.area / 1.0e6f,
                         river.reachesSea ? "reaches the sea"
                         : joins          ? "joins another course"
                                          : "ends at the routing rim");
            }
        }
        // The ground ACROSS the line at the longest run (16 m steps,
        // -400..400 m): is the run in a trough, on a crest, at a rim?
        {
            const bool vertical =
                std::abs(longestX - view->minX) < 150.0f ||
                std::abs(longestX - view->maxX) < 150.0f;
            const f32 line = vertical
                                 ? (std::abs(longestX - view->minX) < 150.0f
                                        ? view->minX
                                        : view->maxX)
                                 : (std::abs(longestZ - view->minZ) < 150.0f
                                        ? view->minZ
                                        : view->maxZ);
            str profile;
            for (f32 d = -400.0f; d <= 400.0f; d += 32.0f) {
                const f32 x = vertical ? line + d : longestX;
                const f32 z = vertical ? longestZ : line + d;
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%.0f ", view->height(x, z));
                profile += buf;
            }
            LOG_INFO("  ground across the line at the run ({} line {:.0f}, "
                     "-400..+400 m by 32): {}",
                     vertical ? "vertical" : "horizontal", line, profile);
            // The same transect on the ANALYTIC and its components: which
            // layer digs the trough.
            const render::terraingen::ProceduralControls ctl { cp };
            str an, base, plateau, basin, bed, storey;
            for (f32 d = -400.0f; d <= 400.0f; d += 32.0f) {
                const f32 x = vertical ? line + d : longestX;
                const f32 z = vertical ? longestZ : line + d;
                char buf[20];
                std::snprintf(buf, sizeof(buf), "%.0f ",
                              render::terraingen::macroHeightAnalytic(
                                  ctl, params.macro, x, z));
                an += buf;
                const render::terraingen::ControlSample cs = ctl.at(x, z);
                std::snprintf(buf, sizeof(buf), "%.0f ", cs.base);
                base += buf;
                std::snprintf(buf, sizeof(buf), "%.0f ", cs.plateau);
                plateau += buf;
                std::snprintf(buf, sizeof(buf), "%.0f ", cs.basinDepth);
                basin += buf;
                std::snprintf(buf, sizeof(buf), "%.0f ", cs.bedDepth);
                bed += buf;
                const render::terraingen::ZoneSample zs =
                    render::terraingen::zoneSampleAt(cp.world, cp.zones, x, z);
                std::snprintf(buf, sizeof(buf), "%.0f ", zs.storeyHeight);
                storey += buf;
            }
            LOG_INFO("  analytic: {}", an);
            LOG_INFO("  base: {}", base);
            LOG_INFO("  plateau (POI lift): {}", plateau);
            LOG_INFO("  basinDepth: {}", basin);
            LOG_INFO("  bedDepth: {}", bed);
            LOG_INFO("  zone storey: {}", storey);
        }
        // The four border lines' resolved styles (a Sea arm IS a water
        // channel along the line).
        render::terraingen::MapGridSpec grid;
        grid.valid = true;
        grid.seed = params.worldSeed;
        grid.mapSize = view->mapSize;
        grid.seaLevel = params.macro.seaLevel;
        const render::terraingen::ProceduralControls controls { cp };
        const auto styleName = [](render::terraingen::MapEdgeStyle st) {
            return st == render::terraingen::MapEdgeStyle::Sea ? "Sea" : "Ridges";
        };
        LOG_INFO("  border lines: west {} | east {} | south {} | north {}",
                 styleName(render::terraingen::mapBorderStyleResolved(
                     controls, params.macro, grid, mapX, mapZ, true)),
                 styleName(render::terraingen::mapBorderStyleResolved(
                     controls, params.macro, grid, mapX + 1, mapZ, true)),
                 styleName(render::terraingen::mapBorderStyleResolved(
                     controls, params.macro, grid, mapZ, mapX, false)),
                 styleName(render::terraingen::mapBorderStyleResolved(
                     controls, params.macro, grid, mapZ + 1, mapX, false)));
    }
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
