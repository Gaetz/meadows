#include "LandscapeReport.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <unordered_map>

#include <stb_image_write.h>

#include "data/forms/LandscapeForms.hpp"
#include "data/plugins/PluginConfig.hpp"
#include "data/plugins/Resolver.hpp"
#include "engine/core/Jobs.hpp"
#include "engine/core/Log.hpp"
#include "engine/render/landscape/TerrainSystem.hpp"
#include "engine/terrain/TerrainNoise.hpp"
#include "engine/terrain/generation/ContinentGround.hpp"
#include "engine/terrain/generation/MapExport.hpp"
#include "engine/terrain/generation/MasterNetwork.hpp"
#include "engine/terrain/generation/PoiPlan.hpp"
#include "engine/terrain/generation/TerrainGen.hpp"
#include "engine/terrain/generation/WorldLayer.hpp"
#include "engine/terrain/generation/ZonePlan.hpp"
#include "game/AllForms.hpp"
#include "game/MapBaker.hpp"
#include "game/MapView.hpp"
#include "game/TerrainBakeStreamer.hpp"
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
    // POI visibility (% of the walkable ground, reportPoiVisibility).
    f64 seeGrand { 0.0 };
    f64 seeMoyen { 0.0 };
    f64 seeBoth { 0.0 };
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

// The game's controls under the world seed: the analytic every
// instrument samples. `cp.world.seed` keeps the controls' own value —
// only ProceduralControls rewrites it, and the plan/zone lookups that
// take `cp.world` directly must see the same field the game hands them.
render::terraingen::ProceduralControlParams controlParams(
    const TileBakeParams& params) {
    render::terraingen::ProceduralControlParams cp = params.controls;
    cp.seed = params.worldSeed;
    return cp;
}

render::terraingen::MasterNetworkParams networkParams(
    const TileBakeParams& params) {
    render::terraingen::MasterNetworkParams net = params.network;
    net.seaLevel = params.macro.seaLevel;
    net.mapGrid = params.mapGrid;
    return net;
}

// What the instruments share: the game's bake params and the analytic
// built once from them, then the slots the steps fill in order — the
// baked map after the bake, the census the history line repeats, the
// worst river run along a map line (where the transect is cut).
struct ReportContext {
    ReportContext(const TileBakeParams& bakeParams, i32 mx, i32 mz,
                  u32 seed, std::filesystem::path cache)
        : mapX { mx }, mapZ { mz }, terrainSeed { seed },
          cacheDir { std::move(cache) }, params { bakeParams },
          cp { controlParams(bakeParams) }, controls { cp },
          net { networkParams(bakeParams) } {}

    i32 mapX;
    i32 mapZ;
    u32 terrainSeed;
    std::filesystem::path cacheDir;
    const TileBakeParams& params;
    render::terraingen::ProceduralControlParams cp;
    render::terraingen::ProceduralControls controls;
    render::terraingen::MasterNetworkParams net;

    f64 bakeSeconds { 0.0 };
    std::optional<game::MapView> view;
    Vec3 spawn { 0.0f };
    Census census;
    u64 key { 0 };
    f32 longestX { 0.0f };
    f32 longestZ { 0.0f };
    std::optional<Vec2> zoomAt; // --at x z: the local dump
};

// Distance from (x, z) to the nearest of the map's four border lines.
f32 mapLineDistance(const game::MapView& view, f32 x, f32 z) {
    return glm::min(
        glm::min(std::abs(x - view.minX), std::abs(x - view.maxX)),
        glm::min(std::abs(z - view.minZ), std::abs(z - view.maxZ)));
}

// Cold costs of the stage-0 network and the plan's typing (what a
// load pays in the spawn probe, what a bake pays in the imprint):
// measured BEFORE the bake warms the process-wide memos.
void reportColdCosts(const ReportContext& ctx) {
    const TileBakeParams& params = ctx.params;
    const auto& cp = ctx.cp;
    const f32 mapSize =
        params.tileSize * static_cast<f32>(game::kMapTilesPerSide);
    const f32 cx = (static_cast<f32>(ctx.mapX) + 0.5f) * mapSize;
    const f32 cz = (static_cast<f32>(ctx.mapZ) + 0.5f) * mapSize;
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
    const auto rivers = render::terraingen::masterRiversNear(
        ctx.controls, params.macro, ctx.net, cx - 0.5f * mapSize,
        cz - 0.5f * mapSize, cx + 0.5f * mapSize, cz + 0.5f * mapSize);
    LOG_INFO("  cold: masterRiversNear (plan on, the map rect) {:.0f} ms, "
             "{} courses",
             lapMs(), rivers.size());
    render::terraingen::masterRiversNear(
        ctx.controls, params.macro, ctx.net, cx - 0.5f * mapSize,
        cz - 0.5f * mapSize, cx + 0.5f * mapSize, cz + 0.5f * mapSize);
    LOG_INFO("  warm: masterRiversNear {:.1f} ms", lapMs());
}

// Bakes the map when its cache is stale for the game's params.
bool bakeIfStale(ReportContext& ctx) {
    const auto start = std::chrono::steady_clock::now();
    if (game::mapBakedAndValid(ctx.cacheDir, ctx.mapX, ctx.mapZ,
                               game::kMapTilesPerSide, &ctx.params)) {
        return true;
    }
    const i32 mapX = ctx.mapX;
    const i32 mapZ = ctx.mapZ;
    core::JobSystem jobs;
    const game::MapBakeStats stats = game::bakeMap(
        ctx.params, mapX, mapZ, ctx.cacheDir, &jobs, game::kMapTilesPerSide,
        [&](u32 landed, u32 total) {
            LOG_INFO("landscape-report: map ({}, {}) [{}/{}] slice",
                     mapX, mapZ, landed, total);
        });
    ctx.bakeSeconds = std::chrono::duration<f64>(
                          std::chrono::steady_clock::now() - start)
                          .count();
    if (stats.slicesWritten == 0) {
        LOG_ERROR("landscape-report: bake failed");
        return false;
    }
    return true;
}

bool loadBakedMap(ReportContext& ctx) {
    ctx.view = game::loadMapView(ctx.params, ctx.mapX, ctx.mapZ,
                                 game::kMapTilesPerSide, ctx.cacheDir);
    if (!ctx.view || ctx.view->slicesLoaded == 0) {
        LOG_ERROR("landscape-report: cannot read the baked map");
        return false;
    }
    return true;
}

// The plan render: controls + POI overlay, the map rect + a rim.
void renderPlan(const ReportContext& ctx) {
    const game::MapView& view = *ctx.view;
    render::terraingen::TerrainMapParams mp;
    mp.centerX = view.centreX();
    mp.centerZ = view.centreZ();
    mp.span = view.mapSize + 1000.0f;
    mp.size = 1024;
    mp.drawPoi = true;
    mp.drawGates = true;
    const vector<u8> pixels = render::terraingen::renderTerrainMap(
        ctx.controls, ctx.params.macro, mp);
    const auto png = view.mapDir / "plan.png";
    stbi_write_png(png.string().c_str(), 1024, 1024, 3, pixels.data(),
                   1024 * 3);
    LOG_INFO("landscape-report: plan -> {}", png.string());
}

// The relief render: a hillshade of the PUBLISHED ground — the plan
// draws the analytic, which never shows what the erosion actually
// carved (grid-aligned valleys were invisible on every plan).
void renderRelief(const ReportContext& ctx) {
    const game::MapView& view = *ctx.view;
    constexpr u32 kN = 1024;
    const f32 step = view.mapSize / static_cast<f32>(kN);
    vector<u8> pixels(static_cast<size_t>(kN) * kN * 3);
    for (u32 row = 0; row < kN; ++row) {
        for (u32 col = 0; col < kN; ++col) {
            const f32 x = view.minX + (static_cast<f32>(col) + 0.5f) * step;
            const f32 z = view.minZ + (static_cast<f32>(row) + 0.5f) * step;
            const f32 h = view.height(x, z);
            const f32 hx = view.height(x + step, z) -
                           view.height(x - step, z);
            const f32 hz = view.height(x, z + step) -
                           view.height(x, z - step);
            // Lambert on a fixed NW light, gamma-ish lift.
            const f32 nx = -hx / (2.0f * step);
            const f32 nz = -hz / (2.0f * step);
            const f32 len = std::sqrt(nx * nx + nz * nz + 1.0f);
            const f32 lambert = glm::clamp(
                (nx * -0.55f + nz * -0.55f + 0.63f) / len, 0.0f,
                1.0f);
            u8 r, g, b;
            if (h < view.seaLevel()) {
                r = 30;
                g = 60;
                b = static_cast<u8>(140 + 60 * lambert);
            } else {
                const u8 v = static_cast<u8>(40 + 215 * lambert);
                r = v;
                g = v;
                b = v;
            }
            u8* px = &pixels[(static_cast<size_t>(row) * kN + col) * 3];
            px[0] = r;
            px[1] = g;
            px[2] = b;
        }
    }
    const auto png = view.mapDir / "relief.png";
    stbi_write_png(png.string().c_str(), static_cast<int>(kN),
                   static_cast<int>(kN), 3, pixels.data(),
                   static_cast<int>(kN) * 3);
    LOG_INFO("landscape-report: relief -> {}", png.string());
}

// The analytic's cost per call (the far terrain, the far water, the
// spawn probe and the control lattice all pay it).
void reportAnalyticCosts(const ReportContext& ctx) {
    const game::MapView& view = *ctx.view;
    const TileBakeParams& params = ctx.params;
    const auto& cp = ctx.cp;
    const auto& controls = ctx.controls;
    constexpr u32 kCalls = 20000;
    f32 sink = 0.0f;
    const auto timed = [&](const char* name, auto&& fn) {
        const auto t0 = std::chrono::steady_clock::now();
        for (u32 i = 0; i < kCalls; ++i) {
            const f32 x = view.minX + 400.0f +
                          static_cast<f32>((i * 7919u) % 7000u);
            const f32 z = view.minZ + 400.0f +
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

// The headline: the map's identity and the transect census around the
// spawn (the map-wide half of the census closes the lakes report).
void reportCensus(ReportContext& ctx) {
    const game::MapView& view = *ctx.view;
    ctx.spawn = view.spawn();
    ctx.census = census(view, ctx.spawn);
    ctx.key = game::mapBakeKey(ctx.params, game::kMapTilesPerSide);
    const Census& c = ctx.census;
    const Vec3& spawn = ctx.spawn;
    LOG_INFO("landscape-report: map ({}, {}) seed {} key {:016x} bake {:.0f} s",
             ctx.mapX, ctx.mapZ, ctx.terrainSeed, ctx.key, ctx.bakeSeconds);
    LOG_INFO("  spawn ({:.0f}, {:.0f}, {:.0f})", spawn.x, spawn.y, spawn.z);
    LOG_INFO("  around the spawn: slope {:.1f} %, steps > 30 deg {:.1f} %, "
             "> 45 deg {:.1f} %, walls {:.1f}/km (median {:.0f} m, tallest "
             "{:.0f} m), relief {:.0f} m / 250 m, rise 100 m p95 {:.0f} m, "
             "ground > 100 m above its 2 km min {:.0f} %",
             c.meanSlopeSpawn, c.steep30, c.steep45, c.wallsPerKm,
             c.wallMedian, c.wallTallest, c.relief250Spawn, c.rise100p95,
             c.highGroundSpawn);
}

void reportLakes(const ReportContext& ctx) {
    const game::MapView& view = *ctx.view;
    const Census& c = ctx.census;
    u32 natural = 0, ponds = 0;
    for (const auto& lake : view.lakes) {
        (lake.dug ? ponds : natural) += 1;
    }
    // The biggest lakes, measured against the FINAL ground:
    // level vs the deepest floor under the mask, and how much of
    // the mask the published terrain has since OUTGROWN (stale) or
    // fallen far below (the sheet overhangs a gorge).
    struct Big {
        const render::terraingen::Lake* lake;
        u32 cells;
    };
    vector<Big> big;
    for (const auto& lake : view.lakes) {
        u32 cells = 0;
        for (const u8 m : lake.mask) {
            cells += m;
        }
        big.push_back(Big { &lake, cells });
    }
    std::sort(big.begin(), big.end(),
              [](const Big& a, const Big& b) {
                  return a.cells > b.cells;
              });
    for (size_t i = 0; i < glm::min<size_t>(big.size(), 4); ++i) {
        const auto& lake = *big[i].lake;
        f32 floor = 1.0e9f;
        u32 total = 0, above = 0, deepUnder = 0;
        for (u32 mz = 0; mz < lake.maskHeight; ++mz) {
            for (u32 mx = 0; mx < lake.maskWidth; ++mx) {
                if (!lake.mask[static_cast<size_t>(mz) *
                                   lake.maskWidth +
                               mx]) {
                    continue;
                }
                // Masks are point lattices (min + m*texel).
                const f32 x = lake.minX +
                              static_cast<f32>(mx) *
                                  lake.maskTexel;
                const f32 z = lake.minZ +
                              static_cast<f32>(mz) *
                                  lake.maskTexel;
                const f32 g = view.height(x, z);
                floor = glm::min(floor, g);
                ++total;
                above += g > lake.level + 0.5f ? 1 : 0;
                deepUnder += g < lake.level - 15.0f ? 1 : 0;
            }
        }
        LOG_INFO("  lake #{}: level {:.1f}, cells {}, depth max "
                 "{:.1f} m, mask {}x{} at ({:.0f}, {:.0f}); final "
                 "ground ABOVE level {} % of mask, 15 m+ under "
                 "{} %",
                 i, lake.level, big[i].cells, lake.level - floor,
                 lake.maskWidth, lake.maskHeight, lake.minX,
                 lake.minZ,
                 total ? 100 * above / total : 0,
                 total ? 100 * deepUnder / total : 0);
    }
    LOG_INFO("  map: relief {:.0f} m / 250 m, ground > 100 m {:.0f} %, "
             "lakes {} ({} natural + {} placed ponds), rivers {}",
             c.relief250Map, c.highGroundMap, c.lakes, natural,
             ponds, c.rivers);
}

// Ribbon-continuity gauge: every run's TAIL must hand its water to
// something — the sea, a nappe, another ribbon (junction/resumption),
// or the map window's edge. A tail with none of those is an ORPHAN
// (water vanishing mid-valley); a tail in a nappe with no ribbon
// nearby is a dead-end to watch (the nappe may be terminal by design).
void reportOrphanTails(const ReportContext& ctx) {
    const game::MapView& view = *ctx.view;
    const auto& lakes = view.lakes;
    const auto inNappe = [&](f32 x, f32 z) {
        for (const auto& lake : lakes) {
            if (lake.mask.empty() || x < lake.minX - 24.0f ||
                x > lake.maxX + 24.0f || z < lake.minZ - 24.0f ||
                z > lake.maxZ + 24.0f) {
                continue;
            }
            const u32 mx = glm::min(
                static_cast<u32>(glm::max(
                    (x - lake.minX) / lake.maskTexel, 0.0f)),
                lake.maskWidth - 1);
            const u32 mz = glm::min(
                static_cast<u32>(glm::max(
                    (z - lake.minZ) / lake.maskTexel, 0.0f)),
                lake.maskHeight - 1);
            if (lake.mask[static_cast<size_t>(mz) * lake.maskWidth +
                          mx]) {
                return true;
            }
        }
        return false;
    };
    const f32 mapMinX = view.minX;
    const f32 mapMinZ = view.minZ;
    const f32 mapMaxX = view.maxX;
    const f32 mapMaxZ = view.maxZ;
    u32 orphans = 0, deadEnds = 0, logged = 0;
    for (size_t ri = 0; ri < view.rivers.size(); ++ri) {
        const auto& river = view.rivers[ri];
        if (river.points.size() < 2) {
            continue;
        }
        const auto& tail = river.points.back();
        if (tail.surface <= view.params.macro.seaLevel + 1.0f) {
            continue; // reaches the sea
        }
        if (tail.x < mapMinX + 80.0f || tail.x > mapMaxX - 80.0f ||
            tail.z < mapMinZ + 80.0f || tail.z > mapMaxZ - 80.0f) {
            continue; // the window edge: the neighbour continues it
        }
        const bool nappe = inNappe(tail.x, tail.z);
        bool continued = false;
        for (size_t rj = 0;
             rj < view.rivers.size() && !continued; ++rj) {
            if (rj == ri) {
                continue;
            }
            for (const auto& pt : view.rivers[rj].points) {
                const f32 dx = pt.x - tail.x;
                const f32 dz = pt.z - tail.z;
                if (dx * dx + dz * dz <
                    (pt.halfWidth + 64.0f) *
                        (pt.halfWidth + 64.0f)) {
                    continued = true;
                    break;
                }
            }
        }
        if (continued) {
            continue;
        }
        if (nappe) {
            ++deadEnds;
            continue;
        }
        ++orphans;
        if (logged < 4) {
            ++logged;
            LOG_INFO("    orphan tail: run {} tier {} at "
                     "({:.0f}, {:.0f}) surf {:.1f} hw {:.1f}",
                     ri, river.tier, tail.x, tail.z, tail.surface,
                     tail.halfWidth);
        }
    }
    LOG_INFO("  water audit: {} orphan tails, {} nappe dead-ends "
             "({} runs)",
             orphans, deadEnds, view.rivers.size());
}

// The CONTINENT summary: the host super-region's master courses —
// does the water of this country reach its sea? A course counts
// transitively: it reaches the sea itself or joins one that does
// (confluences end a trace by design).
void reportContinent(const ReportContext& ctx) {
    const game::MapView& view = *ctx.view;
    const TileBakeParams& params = ctx.params;
    const auto& net = ctx.net;
    const f32 superSize = net.superRegionSize;
    const i32 sx = static_cast<i32>(
        std::floor((view.minX + 1.0f) / superSize));
    const i32 sz = static_cast<i32>(
        std::floor((view.minZ + 1.0f) / superSize));
    const auto courses = render::terraingen::masterRiversNear(
        ctx.controls, params.macro,
        net, static_cast<f32>(sx) * superSize,
        static_cast<f32>(sz) * superSize,
        static_cast<f32>(sx + 1) * superSize,
        static_cast<f32>(sz + 1) * superSize);
    // Only the courses BORN in this continent's core: the rect
    // also catches the neighbours' tails, truncated by the
    // selection — counting them as "lost" is the instrument's
    // bias, not the world's.
    const f32 coreMinX = static_cast<f32>(sx) * superSize +
                         net.superOrigin;
    const f32 coreMinZ = static_cast<f32>(sz) * superSize +
                         net.superOrigin;
    vector<u8> mine(courses.size(), 0);
    for (size_t i = 0; i < courses.size(); ++i) {
        if (courses[i].nodes.empty()) {
            continue;
        }
        const auto& head = courses[i].nodes.front();
        mine[i] = head.x >= coreMinX &&
                  head.x < coreMinX + superSize &&
                  head.z >= coreMinZ &&
                  head.z < coreMinZ + superSize;
    }
    vector<u8> toSea(courses.size(), 0);
    for (size_t i = 0; i < courses.size(); ++i) {
        toSea[i] = courses[i].reachesSea ? 1 : 0;
    }
    // The box-smooth moves a parent course's nodes by up to
    // ~130 m: the tributary's EXACT junction node needs the wider
    // radius to find it again.
    const f32 join2 = 320.0f * 320.0f;
    for (bool grew = true; grew;) {
        grew = false;
        for (size_t i = 0; i < courses.size(); ++i) {
            if (toSea[i] || courses[i].nodes.empty()) {
                continue;
            }
            const auto& tail = courses[i].nodes.back();
            for (size_t j = 0; j < courses.size() && !toSea[i];
                 ++j) {
                if (j == i || !toSea[j]) {
                    continue;
                }
                for (const auto& node : courses[j].nodes) {
                    const f32 dx = node.x - tail.x;
                    const f32 dz = node.z - tail.z;
                    if (dx * dx + dz * dz <= join2) {
                        toSea[i] = 1;
                        grew = true;
                        break;
                    }
                }
            }
        }
    }
    u32 sea = 0, counted = 0, heads[4] = {}, ruisseau = 0,
        riviere = 0, fleuve = 0;
    f32 longest = 0.0f;
    for (size_t i = 0; i < courses.size(); ++i) {
        if (!mine[i]) {
            continue;
        }
        ++counted;
        sea += toSea[i];
        f32 length = 0.0f;
        for (size_t k = 1; k < courses[i].nodes.size(); ++k) {
            length += std::hypot(courses[i].nodes[k].x -
                                     courses[i].nodes[k - 1].x,
                                 courses[i].nodes[k].z -
                                     courses[i].nodes[k - 1].z);
        }
        longest = glm::max(longest, length);
        if (!courses[i].nodes.empty()) {
            ++heads[glm::min(courses[i].nodes.front().headType, 3u)];
        }
        for (const auto& node : courses[i].nodes) {
            if (node.qArea >= params.hydrology.fleuveArea) {
                ++fleuve;
            } else if (node.qArea >= params.hydrology.riviereArea) {
                ++riviere;
            } else {
                ++ruisseau;
            }
        }
    }
    LOG_INFO("  continent (super {}, {}): {} master courses, {} to "
             "the sea ({:.0f} %), longest {:.1f} km; nodes by tier "
             "r/R/F {}/{}/{}; heads spring/neve/glacier/lake "
             "{}/{}/{}/{}",
             sx, sz, counted, sea,
             counted == 0 ? 0.0f
                          : 100.0f * static_cast<f32>(sea) /
                                static_cast<f32>(counted),
             longest / 1000.0f, ruisseau, riviere, fleuve, heads[0],
             heads[1], heads[2], heads[3]);
}

// Where the lakes sit against the zones: on a wall's foot or rim (a
// hollow the storey step traps), on a piece pond, or elsewhere (the
// hydrology's own).
void reportLakesVsZones(const ReportContext& ctx) {
    const game::MapView& view = *ctx.view;
    const auto& cp = ctx.cp;
    u32 nearWall = 0, onPond = 0, other = 0;
    for (const auto& lake : view.lakes) {
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
}

// Walls and gates: a 16 m raster of the map; gate blobs and wall
// stretches between them (4-connected components), the longest
// stretch without a passage (the rule: none over 400 m).
void reportWallsAndGates(const ReportContext& ctx) {
    const game::MapView& view = *ctx.view;
    const auto& cp = ctx.cp;
    const f32 step = 16.0f;
    const u32 n = static_cast<u32>(view.mapSize / step) + 1;
    vector<u8> kind(static_cast<size_t>(n) * n, 0); // 1 wall, 2 gate, 3 ridge
    vector<u8> gateKind(kind.size(), 0);
    u32 notch = 0, breach = 0, col = 0;
    for (u32 row = 0; row < n; ++row) {
        for (u32 c = 0; c < n; ++c) {
            const render::terraingen::ZoneSample zs =
                render::terraingen::zoneSampleAt(
                    cp.world, cp.zones, view.minX + c * step,
                    view.minZ + row * step);
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

// The POI VISIBILITY (docs/POI-CATALOGUE.md §G, docs/PAYSAGE.md §4 bis
// principle 6): from how much of the walkable ground does one see the
// map's grand and a moyen; what each moyen summit sees; does a walk hide
// its destination before revealing it (the triangle). Lines of sight on
// the PUBLISHED ground at 16 m (what the player sees, erosion included),
// eye 1.7 m, targets = the summits of the sites as baked (the highest
// published point inside the kernel's core). Sites outside the map rect
// are not targets: the far terrain of a neighbour map is measured on
// that map. Writes visibility.png next to plan.png.
namespace vis {

struct Raster {
    f32 minX { 0.0f };
    f32 minZ { 0.0f };
    f32 step { 16.0f };
    u32 n { 0 };
    vector<f32> h;

    f32 at(u32 r, u32 c) const { return h[static_cast<size_t>(r) * n + c]; }
    // Bilinear height; outside the raster = the nearest texel.
    f32 sample(f32 x, f32 z) const {
        const f32 fx = glm::clamp((x - minX) / step, 0.0f,
                                  static_cast<f32>(n - 1) - 1.0e-3f);
        const f32 fz = glm::clamp((z - minZ) / step, 0.0f,
                                  static_cast<f32>(n - 1) - 1.0e-3f);
        const u32 c = static_cast<u32>(fx);
        const u32 r = static_cast<u32>(fz);
        const f32 tx = fx - static_cast<f32>(c);
        const f32 tz = fz - static_cast<f32>(r);
        const f32 a = glm::mix(at(r, c), at(r, c + 1), tx);
        const f32 b = glm::mix(at(r + 1, c), at(r + 1, c + 1), tx);
        return glm::mix(a, b, tz);
    }
    // Slope (rise / run) from central differences.
    f32 slope(f32 x, f32 z) const {
        const f32 hx = sample(x + step, z) - sample(x - step, z);
        const f32 hz = sample(x, z + step) - sample(x, z - step);
        return std::sqrt(hx * hx + hz * hz) / (2.0f * step);
    }
};

Raster rasterOf(const game::MapView& view, f32 step) {
    Raster r;
    r.minX = view.minX;
    r.minZ = view.minZ;
    r.step = step;
    r.n = static_cast<u32>(view.mapSize / step) + 1;
    r.h.resize(static_cast<size_t>(r.n) * r.n);
    for (u32 row = 0; row < r.n; ++row) {
        for (u32 c = 0; c < r.n; ++c) {
            r.h[static_cast<size_t>(row) * r.n + c] =
                view.height(r.minX + c * step, r.minZ + row * step);
        }
    }
    return r;
}

// Eye (ex, ez, ey) sees the target (tx, tz, ty): the ray stays above
// the ground, marched at 8 m. The first and last 16 m are skipped —
// the eye stands on the ground, the target IS the ground.
bool sees(const Raster& r, f32 ex, f32 ez, f32 ey, f32 tx, f32 tz, f32 ty) {
    const f32 dx = tx - ex;
    const f32 dz = tz - ez;
    const f32 len = std::sqrt(dx * dx + dz * dz);
    if (len < 32.0f) {
        return true;
    }
    constexpr f32 kStep = 8.0f;
    for (f32 d = 16.0f; d < len - 16.0f; d += kStep) {
        const f32 t = d / len;
        const f32 ground = r.sample(ex + dx * t, ez + dz * t);
        if (ground > glm::mix(ey, ty, t) + 0.5f) {
            return false;
        }
    }
    return true;
}

// A site that stands up: its summit as baked (the highest published
// point within 30 % of its footprint radius of the centre).
struct Target {
    render::terraingen::PoiSite site;
    f32 x { 0.0f };
    f32 z { 0.0f };
    f32 h { 0.0f }; // summit height
};

std::optional<Target> targetOf(const Raster& r,
                               const render::terraingen::PoiSite& s,
                               f32 seaLevel) {
    using render::terraingen::KernelShape;
    using render::terraingen::poiTraits;
    // Basins, pads, springs and mounds have no silhouette: not targets.
    const KernelShape shape = poiTraits(s.type).shape;
    if (s.height < 30.0f || shape == KernelShape::Basin ||
        shape == KernelShape::Pad || shape == KernelShape::Mound ||
        shape == KernelShape::Trench) {
        return std::nullopt;
    }
    Target t;
    t.site = s;
    t.x = s.x;
    t.z = s.z;
    t.h = -1.0e9f;
    const f32 core = glm::min(0.3f * s.radius, 400.0f);
    for (f32 z = s.z - core; z <= s.z + core; z += r.step) {
        for (f32 x = s.x - core; x <= s.x + core; x += r.step) {
            if ((x - s.x) * (x - s.x) + (z - s.z) * (z - s.z) > core * core) {
                continue;
            }
            const f32 h = r.sample(x, z);
            if (h > t.h) {
                t.h = h;
                t.x = x;
                t.z = z;
            }
        }
    }
    if (t.h < seaLevel + 1.0f) {
        return std::nullopt;
    }
    return t;
}

// Shaded ground with the visibility class painted over the walkable
// lattice, the targets as discs.
void writePng(const game::MapView& view, const Raster& r,
              const vector<u8>& cls, u32 latticeN, f32 latticeStep,
              const vector<Target>& grands, const vector<Target>& moyens) {
    constexpr u32 kN = 1024;
    const f32 px = view.mapSize / static_cast<f32>(kN);
    vector<u8> pixels(static_cast<size_t>(kN) * kN * 3);
    for (u32 row = 0; row < kN; ++row) {
        for (u32 col = 0; col < kN; ++col) {
            const f32 x = view.minX + (static_cast<f32>(col) + 0.5f) * px;
            const f32 z = view.minZ + (static_cast<f32>(row) + 0.5f) * px;
            const f32 h = r.sample(x, z);
            const f32 hx = r.sample(x + r.step, z) - r.sample(x - r.step, z);
            const f32 hz = r.sample(x, z + r.step) - r.sample(x, z - r.step);
            const f32 nx = -hx / (2.0f * r.step);
            const f32 nz = -hz / (2.0f * r.step);
            const f32 len = std::sqrt(nx * nx + nz * nz + 1.0f);
            const f32 lambert = glm::clamp(
                (nx * -0.55f + nz * -0.55f + 0.63f) / len, 0.0f, 1.0f);
            const f32 shade = 0.45f + 0.55f * lambert;
            glm::vec3 tint { 0.55f, 0.55f, 0.55f }; // not walkable
            if (h < view.seaLevel()) {
                tint = { 0.12f, 0.25f, 0.6f };
            } else {
                const u32 lc = glm::min(latticeN - 1,
                                        static_cast<u32>((x - view.minX) /
                                                         latticeStep));
                const u32 lr = glm::min(latticeN - 1,
                                        static_cast<u32>((z - view.minZ) /
                                                         latticeStep));
                switch (cls[static_cast<size_t>(lr) * latticeN + lc]) {
                case 1: tint = { 0.75f, 0.15f, 0.15f }; break; // sees none
                case 2: tint = { 0.95f, 0.8f, 0.2f }; break;   // moyen only
                case 3: tint = { 0.25f, 0.55f, 0.95f }; break; // grand only
                case 4: tint = { 0.3f, 0.75f, 0.3f }; break;   // both
                default: break;
                }
            }
            u8* p = &pixels[(static_cast<size_t>(row) * kN + col) * 3];
            p[0] = static_cast<u8>(255.0f * glm::clamp(tint.x * shade, 0.0f, 1.0f));
            p[1] = static_cast<u8>(255.0f * glm::clamp(tint.y * shade, 0.0f, 1.0f));
            p[2] = static_cast<u8>(255.0f * glm::clamp(tint.z * shade, 0.0f, 1.0f));
        }
    }
    const auto disc = [&](f32 x, f32 z, i32 radius, u8 rr, u8 gg, u8 bb) {
        const i32 cc = static_cast<i32>((x - view.minX) / px);
        const i32 cr = static_cast<i32>((z - view.minZ) / px);
        for (i32 dr = -radius; dr <= radius; ++dr) {
            for (i32 dc = -radius; dc <= radius; ++dc) {
                const i32 d2 = dr * dr + dc * dc;
                if (d2 > radius * radius) {
                    continue;
                }
                const i32 row = cr + dr, col = cc + dc;
                if (row < 0 || col < 0 || row >= static_cast<i32>(kN) ||
                    col >= static_cast<i32>(kN)) {
                    continue;
                }
                u8* p = &pixels[(static_cast<size_t>(row) * kN + col) * 3];
                const bool rim = d2 > (radius - 2) * (radius - 2);
                p[0] = rim ? 0 : rr;
                p[1] = rim ? 0 : gg;
                p[2] = rim ? 0 : bb;
            }
        }
    };
    for (const Target& t : moyens) {
        disc(t.x, t.z, 6, 255, 255, 255);
    }
    for (const Target& t : grands) {
        disc(t.x, t.z, 11, 255, 255, 255);
    }
    const auto png = view.mapDir / "visibility.png";
    stbi_write_png(png.string().c_str(), static_cast<int>(kN),
                   static_cast<int>(kN), 3, pixels.data(),
                   static_cast<int>(kN) * 3);
    LOG_INFO("landscape-report: visibility -> {}", png.string());
}

} // namespace vis

void reportPoiVisibility(ReportContext& ctx) {
    using render::terraingen::PoiTier;
    const game::MapView& view = *ctx.view;
    const auto& cp = ctx.cp;
    const vis::Raster r = vis::rasterOf(view, 16.0f);
    constexpr f32 kEye = 1.7f;
    constexpr f32 kMoyenReach = 3000.0f; // the goal of the next walk

    // The targets: the map's grands and moyens that stand up.
    const auto sites = render::terraingen::poiSitesNear(
        cp.world, cp.poi, view.minX, view.minZ, view.maxX, view.maxZ);
    vector<vis::Target> grands, moyens;
    u32 grandSites = 0, moyenSites = 0;
    for (const auto& s : sites) {
        if (s.tier == PoiTier::Petit || !view.inside(s.x, s.z)) {
            continue;
        }
        (s.tier == PoiTier::Grand ? grandSites : moyenSites) += 1;
        const auto t = vis::targetOf(r, s, view.seaLevel());
        if (t) {
            (s.tier == PoiTier::Grand ? grands : moyens).push_back(*t);
        }
        // The site as planned and as baked: its summit, and how far it
        // stands above the ring at 1.2 radius (prominence).
        const f32 sx = t ? t->x : s.x;
        const f32 sz = t ? t->z : s.z;
        const f32 summit = r.sample(sx, sz);
        f32 ring = 0.0f;
        for (u32 k = 0; k < 8; ++k) {
            const f32 a = static_cast<f32>(k) * 0.7853982f;
            ring += r.sample(s.x + 1.2f * s.radius * std::cos(a),
                             s.z + 1.2f * s.radius * std::sin(a));
        }
        ring *= 0.125f;
        LOG_INFO("    {} {} at ({:.0f}, {:.0f}) r {:.0f} m lift {:.0f} m: "
                 "baked summit {:.0f} m, prominence {:.0f} m{}",
                 s.tier == PoiTier::Grand ? "grand" : "moyen",
                 render::terraingen::poiTypeName(s.type), s.x, s.z,
                 s.radius, s.height, summit, summit - ring,
                 t ? "" : " (no silhouette)");
    }

    // The walkable lattice: dry land under 30 deg, 32 m apart.
    const f32 step = 32.0f;
    const u32 n = static_cast<u32>(view.mapSize / step);
    vector<u8> cls(static_cast<size_t>(n) * n, 0); // 0 n/a, 1 none, 2 moyen, 3 grand, 4 both
    u32 land = 0, walkable = 0, seeGrand = 0, seeMoyen = 0, seeBoth = 0;
    u32 nearestSeen = 0;
    u32 byBand[3] = { 0, 0, 0 }, inBand[3] = { 0, 0, 0 }; // grand at < 2, 2-4, > 4 km
    for (u32 row = 0; row < n; ++row) {
        for (u32 c = 0; c < n; ++c) {
            const f32 x = view.minX + (static_cast<f32>(c) + 0.5f) * step;
            const f32 z = view.minZ + (static_cast<f32>(row) + 0.5f) * step;
            const f32 h = r.sample(x, z);
            if (h <= view.seaLevel() + 1.0f) {
                continue;
            }
            ++land;
            if (r.slope(x, z) > 0.5774f || view.wet(x, z)) {
                continue;
            }
            ++walkable;
            const f32 eye = h + kEye;
            bool grand = false;
            f32 dGrand = 1.0e9f;
            for (const vis::Target& t : grands) {
                dGrand = glm::min(dGrand, std::hypot(t.x - x, t.z - z));
                grand = grand || vis::sees(r, x, z, eye, t.x, t.z, t.h);
            }
            bool moyen = false;
            const vis::Target* nearest = nullptr;
            f32 dNearest = 1.0e9f;
            for (const vis::Target& t : moyens) {
                const f32 d = std::hypot(t.x - x, t.z - z);
                if (d < dNearest) {
                    dNearest = d;
                    nearest = &t;
                }
                if (d <= kMoyenReach && !moyen) {
                    moyen = vis::sees(r, x, z, eye, t.x, t.z, t.h);
                }
            }
            if (nearest && vis::sees(r, x, z, eye, nearest->x, nearest->z,
                                     nearest->h)) {
                ++nearestSeen;
            }
            seeGrand += grand;
            seeMoyen += moyen;
            seeBoth += grand && moyen;
            if (!grands.empty()) {
                const u32 band = dGrand < 2000.0f ? 0 : dGrand < 4000.0f ? 1 : 2;
                ++inBand[band];
                byBand[band] += grand;
            }
            cls[static_cast<size_t>(row) * n + c] =
                grand && moyen ? 4 : grand ? 3 : moyen ? 2 : 1;
        }
    }
    const f64 wk = static_cast<f64>(glm::max(walkable, 1u));
    LOG_INFO("  poi visibility: {} grands ({} stand up), {} moyens ({} stand "
             "up); walkable {:.0f} % of the land ({} points at 32 m)",
             grandSites, grands.size(), moyenSites, moyens.size(),
             100.0 * walkable / glm::max(land, 1u), walkable);
    LOG_INFO("  poi visibility: from the walkable ground — the grand {:.1f} %, "
             "a moyen within 3 km {:.1f} %, both {:.1f} % (target 90), the "
             "nearest moyen {:.1f} %; the grand by distance < 2 km {:.0f} %, "
             "2-4 km {:.0f} %, > 4 km {:.0f} %",
             100.0 * seeGrand / wk, 100.0 * seeMoyen / wk,
             100.0 * seeBoth / wk, 100.0 * nearestSeen / wk,
             100.0 * byBand[0] / glm::max(inBand[0], 1u),
             100.0 * byBand[1] / glm::max(inBand[1], 1u),
             100.0 * byBand[2] / glm::max(inBand[2], 1u));

    // The summit rewards: from each moyen summit, >= 2 other moyens and
    // the grand (>= 90 % of the summits).
    u32 summitsOk = 0, summitsGrand = 0;
    vector<f32> seenCounts;
    for (const vis::Target& s : moyens) {
        const f32 eye = s.h + kEye;
        u32 seen = 0;
        for (const vis::Target& t : moyens) {
            if (&t == &s) {
                continue;
            }
            seen += vis::sees(r, s.x, s.z, eye, t.x, t.z, t.h);
        }
        bool grand = false;
        for (const vis::Target& t : grands) {
            grand = grand || vis::sees(r, s.x, s.z, eye, t.x, t.z, t.h);
        }
        seenCounts.push_back(static_cast<f32>(seen));
        summitsGrand += grand;
        summitsOk += grand && seen >= 2;
    }
    LOG_INFO("  poi visibility: from the moyen summits — other moyens seen "
             "median {:.0f}, the grand {} / {}, >= 2 moyens + the grand {} / "
             "{} ({:.0f} %, target 90)",
             median(seenCounts), summitsGrand, moyens.size(), summitsOk,
             moyens.size(),
             100.0 * summitsOk / glm::max<size_t>(moyens.size(), 1));

    // The triangle along the walks: from the ground of the walk a -> w
    // -> b (eye 1.7 m), is b's summit seen at the start, lost on the
    // way, seen again before arrival. Both directions of every edge
    // whose ends both stand up.
    const auto edges = render::terraingen::poiEdgesNear(
        cp.world, cp.poi, view.minX, view.minZ, view.maxX, view.maxZ);
    const auto targetFor = [&](const render::terraingen::PoiSite& s) {
        for (const vis::Target& t : moyens) {
            if (t.site.cellX == s.cellX && t.site.cellZ == s.cellZ) {
                return static_cast<const vis::Target*>(&t);
            }
        }
        return static_cast<const vis::Target*>(nullptr);
    };
    u32 walks = 0, hideReveal = 0, alwaysSeen = 0, neverSeen = 0, lostAtEnd = 0;
    const auto walk = [&](const render::terraingen::PoiSite& a,
                          const render::terraingen::PoiSite& b, f32 wx,
                          f32 wz) {
        const vis::Target* tb = targetFor(b);
        if (!tb || !view.inside(a.x, a.z) || !view.inside(b.x, b.z)) {
            return;
        }
        // The polyline a -> w -> b, from a's footprint edge to b's.
        const f32 l1 = std::hypot(wx - a.x, wz - a.z);
        const f32 l2 = std::hypot(b.x - wx, b.z - wz);
        const f32 total = l1 + l2;
        const f32 from = glm::min(a.radius, 0.4f * total);
        const f32 to = total - glm::min(0.5f * b.radius, 0.4f * total);
        if (to - from < 200.0f) {
            return;
        }
        constexpr u32 kSamples = 40;
        bool seenEarly = false, lostMid = false, seenLate = false, any = false;
        for (u32 i = 0; i <= kSamples; ++i) {
            const f32 u = static_cast<f32>(i) / kSamples;
            const f32 d = glm::mix(from, to, u);
            f32 x, z;
            if (d <= l1) {
                x = glm::mix(a.x, wx, d / glm::max(l1, 1.0f));
                z = glm::mix(a.z, wz, d / glm::max(l1, 1.0f));
            } else {
                x = glm::mix(wx, b.x, (d - l1) / glm::max(l2, 1.0f));
                z = glm::mix(wz, b.z, (d - l1) / glm::max(l2, 1.0f));
            }
            const bool seen =
                vis::sees(r, x, z, r.sample(x, z) + kEye, tb->x, tb->z, tb->h);
            any = any || seen;
            if (u <= 0.2f) {
                seenEarly = seenEarly || seen;
            } else if (u < 0.8f) {
                lostMid = lostMid || !seen;
            } else {
                seenLate = seenLate || seen;
            }
        }
        ++walks;
        if (!any) {
            ++neverSeen;
        } else if (seenEarly && lostMid && seenLate) {
            ++hideReveal;
        } else if (!lostMid && seenEarly && seenLate) {
            ++alwaysSeen;
        } else if (!seenLate) {
            ++lostAtEnd;
        }
    };
    for (const auto& e : edges) {
        walk(e.a, e.b, e.wx, e.wz);
        walk(e.b, e.a, e.wx, e.wz);
    }
    LOG_INFO("  poi visibility: the walks — {} measured, hide then reveal "
             "{} ({:.0f} %, target 80), always in view {}, never in view {}, "
             "hidden on arrival {}",
             walks, hideReveal, 100.0 * hideReveal / glm::max(walks, 1u),
             alwaysSeen, neverSeen, lostAtEnd);

    ctx.census.seeGrand = 100.0 * seeGrand / wk;
    ctx.census.seeMoyen = 100.0 * seeMoyen / wk;
    ctx.census.seeBoth = 100.0 * seeBoth / wk;
    vis::writePng(view, r, cls, n, step, grands, moyens);
}

// Rivers hugging a map line: nodes within 150 m of the map's four
// border lines, per tier, and the longest run of consecutive nodes
// that stays there. Leaves the worst run's last node in the context
// for the across-the-line transect.
void reportRiversVsMapLines(ReportContext& ctx) {
    const game::MapView& view = *ctx.view;
    u32 nodesTotal = 0, nodesNear = 0;
    u32 longestRun = 0;
    u8 longestTier = 0;
    f32 longestX = 0.0f, longestZ = 0.0f;
    // A run's EXTENT along the line (meters between its first and
    // last node, projected on the line) tells a hug from an oblique
    // crossing: a 25-degree crossing stays within 150 m over 650 m
    // without ever running along.
    f32 longestAlong = 0.0f, longestAcross = 0.0f;
    for (const auto& river : view.rivers) {
        u32 run = 0;
        f32 runX0 = 0.0f, runZ0 = 0.0f;
        for (const auto& node : river.points) {
            ++nodesTotal;
            if (mapLineDistance(view, node.x, node.z) < 150.0f) {
                ++nodesNear;
                if (run == 0) {
                    runX0 = node.x;
                    runZ0 = node.z;
                }
                ++run;
                // Which line: the nearer axis decides along/across.
                const f32 dx = glm::min(std::abs(node.x - view.minX),
                                        std::abs(node.x - view.maxX));
                const f32 dz = glm::min(std::abs(node.z - view.minZ),
                                        std::abs(node.z - view.maxZ));
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
             longestZ, view.height(longestX, longestZ), view.seaLevel(),
             longestAlong, longestAcross);
    ctx.longestX = longestX;
    ctx.longestZ = longestZ;
}

// River sections: the published ground cut ACROSS every ribbon. A
// river reads as one channel when the water under its ribbon is one
// wet run; two wet runs with a thin dry strip between them (the groove
// the erosion dug beside the groove the carve dug) is a SPLIT bed. The
// deepest sample's lateral offset from the axis tells how far the
// course sits from its own thalweg; the banks at halfWidth + 30 m tell
// how incised the water runs; the surface drop per ~50 m of course,
// weighted by length, tells whether the water visibly descends.
void reportRiverSections(const ReportContext& ctx) {
    const game::MapView& view = *ctx.view;
    const auto underLake = [&](f32 x, f32 z) {
        for (const auto& lake : view.lakes) {
            if (lake.covers(x, z)) {
                return true;
            }
        }
        return false;
    };
    std::optional<render::terraingen::ContinentGround> continent;
    if (ctx.params.continent.enabled) {
        const auto& net = ctx.params.network;
        continent = render::terraingen::loadContinentGround(
            ctx.cacheDir, ctx.params, view.mapSize,
            render::terraingen::continentSuperIndex(net, view.centreX()),
            render::terraingen::continentSuperIndex(net, view.centreZ()));
    }
    constexpr f32 kWetMargin = 0.2f;   // ground this far under the water
    constexpr f32 kSliverMax = 12.0f;  // dry strip still "thin"
    constexpr f32 kBankOut = 30.0f;    // bank probe beyond the ribbon
    u32 sections = 0, split = 0, sliver = 0, dryInRibbon = 0;
    u32 splitIn = 0, sliverIn = 0; // second channel starting INSIDE the ribbon
    u32 byTier[3] = {}, splitByTier[3] = {};
    vector<f32> offsets;   // |deepest - axis| / halfWidth
    vector<f32> sliverW;   // dry strip width (m) of split sections
    vector<f32> sliverH;   // dry strip crest above the water (m)
    vector<f32> gapM;      // axis-to-second-channel distance (m)
    vector<f32> bankLow;   // the lower bank at hw + 30 m, above water
    vector<f32> bankHigh;  // the higher bank
    vector<f32> ribbonDry; // % of the ribbon's width standing dry
    // Who owns the water near the player (docs/PAYSAGE.md §1.5): the
    // sim pins a course only from kPinHalfWidth of half-width AND
    // while the bed stays within max(9, 1.5 hw) of the water; under
    // the pin width the ribbon is a BROOK — the baked ribbon stays
    // drawn inside the sim window (film relief only). A brook whose
    // bed lies metres under its surface is a ribbon hanging in the
    // air at close range; a brook that was a river a few points
    // upstream is the steep narrowing at work.
    constexpr f32 kPinHalfWidth = 4.0f; // sim default (pinRiverHalfWidth)
    constexpr f32 kHangMin = 3.0f;      // bed this far under = hanging
    u32 brooks = 0, brookHang = 0, brookDeep = 0, brookNarrowed = 0;
    u32 brookNarrowedHang = 0, riverUnpinned = 0, riverDeep = 0;
    vector<f32> brookHangM;
    struct Hang {
        f32 x, z, surface, hw, lawHw, drop;
    };
    vector<Hang> hangs;
    struct Example {
        f32 x, z, surface, hw, gap, width, crest;
        u32 tier;
    };
    vector<Example> examples;
    for (const auto& river : view.rivers) {
        const size_t count = river.points.size();
        if (count < 9) {
            continue;
        }
        for (size_t i = 3; i + 3 < count; i += 2) {
            const auto& pt = river.points[i];
            if (underLake(pt.x, pt.z) || pt.halfWidth < 0.5f) {
                continue;
            }
            if (pt.x < view.minX + 40.0f || pt.x > view.maxX - 40.0f ||
                pt.z < view.minZ + 40.0f || pt.z > view.maxZ - 40.0f) {
                continue;
            }
            const auto& fw = river.points[i + 1];
            const auto& bw = river.points[i - 1];
            const f32 dx = fw.x - bw.x;
            const f32 dz = fw.z - bw.z;
            const f32 len = std::sqrt(dx * dx + dz * dz);
            if (len < 0.5f) {
                continue;
            }
            const f32 px = -dz / len;
            const f32 pz = dx / len;
            const f32 hw = pt.halfWidth;
            const f32 reach = glm::max(2.5f * hw, hw + 16.0f);
            // 1 m samples across; the ribbon is |d| <= hw.
            const i32 half = static_cast<i32>(std::ceil(reach));
            vector<u8> wet(static_cast<size_t>(2 * half + 1), 0);
            f32 deepest = 1.0e30f;
            f32 deepestD = 0.0f;
            u32 ribbonN = 0, ribbonDryN = 0;
            for (i32 k = -half; k <= half; ++k) {
                const f32 d = static_cast<f32>(k);
                const f32 g = view.height(pt.x + px * d, pt.z + pz * d);
                const bool w = g < pt.surface - kWetMargin;
                wet[static_cast<size_t>(k + half)] = w ? 1 : 0;
                if (g < deepest) {
                    deepest = g;
                    deepestD = d;
                }
                if (std::abs(d) <= hw) {
                    ++ribbonN;
                    ribbonDryN += w ? 0 : 1;
                }
            }
            ++sections;
            const u32 tier = glm::min(pt.tier, 2u);
            ++byTier[tier];
            {
                const f32 drop = pt.surface - view.height(pt.x, pt.z);
                if (hw < kPinHalfWidth) {
                    ++brooks;
                    f32 lawHw = hw;
                    for (size_t k = i >= 10 ? i - 10 : 0;
                         k <= glm::min(i + 10, count - 1); ++k) {
                        lawHw = glm::max(lawHw, river.points[k].halfWidth);
                    }
                    const bool narrowed = lawHw >= kPinHalfWidth;
                    brookNarrowed += narrowed ? 1 : 0;
                    if (drop > kHangMin) {
                        ++brookHang;
                        brookHangM.push_back(drop);
                        brookNarrowedHang += narrowed ? 1 : 0;
                        brookDeep += drop > 8.0f ? 1 : 0;
                        if (hangs.size() < 6 && narrowed && drop > 8.0f) {
                            hangs.push_back({ pt.x, pt.z, pt.surface, hw,
                                              lawHw, drop });
                        }
                    }
                } else if (drop > glm::max(9.0f, 1.5f * hw)) {
                    ++riverUnpinned;
                    riverDeep += drop > 20.0f ? 1 : 0;
                }
            }
            offsets.push_back(std::abs(deepestD) / hw);
            if (ribbonN > 0) {
                ribbonDry.push_back(100.0f * static_cast<f32>(ribbonDryN) /
                                    static_cast<f32>(ribbonN));
                if (ribbonDryN > 0) {
                    ++dryInRibbon;
                }
            }
            const f32 gL = view.height(pt.x - px * (hw + kBankOut),
                                       pt.z - pz * (hw + kBankOut)) -
                           pt.surface;
            const f32 gR = view.height(pt.x + px * (hw + kBankOut),
                                       pt.z + pz * (hw + kBankOut)) -
                           pt.surface;
            bankLow.push_back(glm::min(gL, gR));
            bankHigh.push_back(glm::max(gL, gR));
            // Wet runs across: the axis run and the nearest other run.
            i32 axisK = 0;
            if (!wet[static_cast<size_t>(half)]) {
                // Axis dry: the nearest wet sample inside the ribbon
                // stands in for it.
                i32 best = half + 1;
                for (i32 k = -half; k <= half; ++k) {
                    if (wet[static_cast<size_t>(k + half)] &&
                        std::abs(k) <= static_cast<i32>(hw) + 1 &&
                        std::abs(k) < std::abs(best)) {
                        best = k;
                    }
                }
                if (best > half) {
                    continue; // no water under the ribbon at all
                }
                axisK = best;
            }
            i32 runLo = axisK, runHi = axisK;
            while (runLo > -half && wet[static_cast<size_t>(runLo - 1 + half)]) {
                --runLo;
            }
            while (runHi < half && wet[static_cast<size_t>(runHi + 1 + half)]) {
                ++runHi;
            }
            // The nearest other wet run on either side, its dry gap.
            f32 gap = 0.0f, width = 0.0f, crest = 0.0f;
            i32 secondK = 0;
            bool found = false;
            for (const i32 dir : { -1, 1 }) {
                i32 k = dir < 0 ? runLo - 1 : runHi + 1;
                i32 dry = 0;
                f32 top = -1.0e30f;
                while (k >= -half && k <= half &&
                       !wet[static_cast<size_t>(k + half)]) {
                    const f32 d = static_cast<f32>(k);
                    top = glm::max(top,
                                   view.height(pt.x + px * d, pt.z + pz * d) -
                                       pt.surface);
                    ++dry;
                    k += dir;
                }
                if (k < -half || k > half || dry == 0) {
                    continue; // reached the probe's end: one channel
                }
                // A second run must hold real water: 3 m+ wide.
                i32 w2 = 0;
                i32 kk = k;
                while (kk >= -half && kk <= half &&
                       wet[static_cast<size_t>(kk + half)]) {
                    ++w2;
                    kk += dir;
                }
                if (w2 < 3) {
                    continue;
                }
                const f32 dryW = static_cast<f32>(dry);
                if (!found || dryW < width) {
                    found = true;
                    width = dryW;
                    crest = top;
                    secondK = k;
                    gap = std::abs(static_cast<f32>(secondK));
                }
            }
            if (!found) {
                continue;
            }
            ++split;
            ++splitByTier[tier];
            gapM.push_back(gap);
            const bool inside = gap <= hw;
            splitIn += inside ? 1 : 0;
            if (width <= kSliverMax) {
                ++sliver;
                sliverIn += inside ? 1 : 0;
                sliverW.push_back(width);
                sliverH.push_back(crest);
                if (examples.size() < 6 && crest > 0.3f) {
                    examples.push_back({ pt.x, pt.z, pt.surface, hw, gap,
                                         width, crest, tier });
                }
            }
        }
    }
    if (sections == 0) {
        LOG_INFO("  river sections: none measurable");
        return;
    }
    const auto pct = [&](u32 n) {
        return 100.0f * static_cast<f32>(n) / static_cast<f32>(sections);
    };
    LOG_INFO("  river sections ({} cuts, r/R/F {}/{}/{}): split bed {} "
             "({:.1f} %; by tier {}/{}/{}; {} inside the ribbon), thin "
             "sliver (dry strip <= {:.0f} m) {} ({:.1f} %; {} inside the "
             "ribbon), width median {:.1f} m, crest above water median "
             "{:.2f} / p90 {:.2f} m, second channel at median {:.1f} m from "
             "the axis",
             sections, byTier[0], byTier[1], byTier[2], split, pct(split),
             splitByTier[0], splitByTier[1], splitByTier[2], splitIn,
             kSliverMax, sliver, pct(sliver), sliverIn, median(sliverW),
             median(sliverH), percentile(sliverH, 90), median(gapM));
    LOG_INFO("  river sections: deepest sample off the axis median {:.2f} / "
             "p90 {:.2f} halfWidths; ribbon with dry ground {} ({:.1f} %), "
             "dry share of the ribbon median {:.0f} / p90 {:.0f} %; banks "
             "at hw + {:.0f} m above the water: lower median {:.1f} / p90 "
             "{:.1f} m, higher median {:.1f} / p90 {:.1f} m",
             median(offsets), percentile(offsets, 90), dryInRibbon,
             pct(dryInRibbon), median(ribbonDry), percentile(ribbonDry, 90),
             kBankOut, median(bankLow), percentile(bankLow, 90),
             median(bankHigh), percentile(bankHigh, 90));
    LOG_INFO("  sim ownership (pin >= {:.0f} m hw, bed <= max(9, 1.5 hw) under): "
             "brooks {} cuts ({:.1f} %), of which narrowed from a river (law "
             "hw >= pin within 10 pts) {} ({:.1f} %); brook ribbons hanging "
             "> {:.0f} m over their bed {} ({:.1f} %; narrowed {}; > 8 m "
             "{}), hang median {:.1f} / p90 {:.1f} m; rivers too deep to "
             "pin {} ({:.1f} %; > 20 m {})",
             kPinHalfWidth, brooks, pct(brooks), brookNarrowed,
             pct(brookNarrowed), kHangMin, brookHang, pct(brookHang),
             brookNarrowedHang, brookDeep, median(brookHangM),
             percentile(brookHangM, 90), riverUnpinned, pct(riverUnpinned),
             riverDeep);
    for (const Hang& h : hangs) {
        LOG_INFO("    hanging brook at ({:.0f}, {:.0f}) surf {:.1f}: hw {:.1f} "
                 "(law {:.1f}), bed {:.1f} m under",
                 h.x, h.z, h.surface, h.hw, h.lawHw, h.drop);
    }
    for (const Example& e : examples) {
        LOG_INFO("    sliver: tier {} at ({:.0f}, {:.0f}) surf {:.1f} hw "
                 "{:.1f}: second channel {:.0f} m off, dry strip {:.0f} m "
                 "wide, crest +{:.2f} m",
                 e.tier, e.x, e.z, e.surface, e.hw, e.gap, e.width,
                 e.crest);
    }
    // Two transverse profiles (ground - water, 2 m steps) for the eye.
    for (size_t n = 0; n < glm::min<size_t>(examples.size(), 2); ++n) {
        const Example& e = examples[n];
        // Re-find the perpendicular from the nearest course point.
        f32 px = 0.0f, pz = 0.0f;
        f32 bestD2 = 1.0e30f;
        for (const auto& river : view.rivers) {
            for (size_t i = 1; i + 1 < river.points.size(); ++i) {
                const auto& pt = river.points[i];
                const f32 d2 = (pt.x - e.x) * (pt.x - e.x) +
                               (pt.z - e.z) * (pt.z - e.z);
                if (d2 < bestD2) {
                    bestD2 = d2;
                    const f32 dx = river.points[i + 1].x - river.points[i - 1].x;
                    const f32 dz = river.points[i + 1].z - river.points[i - 1].z;
                    const f32 len = glm::max(std::sqrt(dx * dx + dz * dz), 0.01f);
                    px = -dz / len;
                    pz = dx / len;
                }
            }
        }
        // Published ground and the continent ground (stage 1, 16 m)
        // across +-160 m: whether the slot the water runs in was dug
        // by the erosion or by the finalize.
        std::string line, cont;
        for (i32 d = -160; d <= 160; d += 8) {
            const f32 x = e.x + px * static_cast<f32>(d);
            const f32 z = e.z + pz * static_cast<f32>(d);
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%+.0f ",
                          view.height(x, z) - e.surface);
            line += buf;
            f32 h = 0.0f;
            if (continent && continent->heightAt(x, z, h)) {
                std::snprintf(buf, sizeof(buf), "%+.0f ", h - e.surface);
            } else {
                std::snprintf(buf, sizeof(buf), "- ");
            }
            cont += buf;
        }
        LOG_INFO("    profile ({:.0f}, {:.0f}) hw {:.0f}, -160..+160 m by 8, "
                 "published - water: {}",
                 e.x, e.z, e.hw, line);
        LOG_INFO("    profile ({:.0f}, {:.0f}) continent - water:      {}",
                 e.x, e.z, cont);
    }
    // Along the two longest fleuves: water, published axis ground,
    // continent axis ground and the lower bank, every ~250 m — the
    // flats and falls of the profile against the floor they ride.
    {
        vector<size_t> fleuves;
        for (size_t r = 0; r < view.rivers.size(); ++r) {
            const auto& river = view.rivers[r];
            if (river.points.size() > 40 && river.tier == 2) {
                fleuves.push_back(r);
            }
        }
        std::sort(fleuves.begin(), fleuves.end(), [&](size_t a, size_t b) {
            return view.rivers[a].points.size() > view.rivers[b].points.size();
        });
        for (size_t n = 0; n < glm::min<size_t>(fleuves.size(), 2); ++n) {
            const auto& river = view.rivers[fleuves[n]];
            std::string line;
            f32 arc = 0.0f, nextAt = 0.0f;
            for (size_t i = 1; i + 1 < river.points.size(); ++i) {
                const auto& pt = river.points[i];
                arc += std::hypot(pt.x - river.points[i - 1].x,
                                  pt.z - river.points[i - 1].z);
                if (arc < nextAt) {
                    continue;
                }
                nextAt += 250.0f;
                const f32 dx = river.points[i + 1].x - river.points[i - 1].x;
                const f32 dz = river.points[i + 1].z - river.points[i - 1].z;
                const f32 len = glm::max(std::sqrt(dx * dx + dz * dz), 0.01f);
                const f32 px = -dz / len;
                const f32 pz = dx / len;
                const f32 out = pt.halfWidth + 30.0f;
                const f32 bank = glm::min(
                    view.height(pt.x - px * out, pt.z - pz * out),
                    view.height(pt.x + px * out, pt.z + pz * out));
                f32 c = 0.0f;
                const bool hasC = continent && continent->heightAt(pt.x, pt.z, c);
                char buf[96];
                std::snprintf(buf, sizeof(buf), "[%.1fkm w%.0f g%+.0f c%s%.0f b%+.0f] ",
                              arc / 1000.0f, pt.surface,
                              view.height(pt.x, pt.z) - pt.surface,
                              hasC ? (c - pt.surface >= 0 ? "+" : "") : "?",
                              hasC ? c - pt.surface : 0.0f, bank - pt.surface);
                line += buf;
            }
            LOG_INFO("  fleuve {} ({} pts, hw {:.0f}-{:.0f} m) along: w water, "
                     "g axis ground - w, c continent - w, b lower bank - w: {}",
                     fleuves[n], river.points.size(),
                     river.points.front().halfWidth,
                     river.points.back().halfWidth, line);
            // Zoom: the longest run of flat water (< 0.1 m per point)
            // with a bank over 20 m, every 3rd point (24 m) — what the
            // slot the flat water runs in is made of.
            {
                size_t bestA = 0, bestB = 0;
                size_t a = 0;
                for (size_t i = 1; i <= river.points.size(); ++i) {
                    bool cont = i < river.points.size();
                    if (cont) {
                        const auto& p = river.points[i];
                        const auto& q = river.points[i - 1];
                        const f32 dx = p.x - q.x, dz = p.z - q.z;
                        const f32 len = glm::max(std::hypot(dx, dz), 0.01f);
                        const f32 out = p.halfWidth + 30.0f;
                        const f32 bank = glm::min(
                            view.height(p.x + dz / len * out, p.z - dx / len * out),
                            view.height(p.x - dz / len * out, p.z + dx / len * out));
                        cont = std::abs(q.surface - p.surface) < 0.1f &&
                               bank - p.surface > 20.0f;
                    }
                    if (!cont) {
                        if (i - a > bestB - bestA) {
                            bestA = a;
                            bestB = i;
                        }
                        a = i;
                    }
                }
                if (bestB > bestA + 6) {
                    std::string zoom;
                    for (size_t i = bestA; i < bestB; i += 3) {
                        const auto& p = river.points[i];
                        const size_t j = i + 1 < river.points.size() ? i + 1 : i;
                        const size_t k = i > 0 ? i - 1 : i;
                        const f32 dx = river.points[j].x - river.points[k].x;
                        const f32 dz = river.points[j].z - river.points[k].z;
                        const f32 len = glm::max(std::hypot(dx, dz), 0.01f);
                        const f32 out = p.halfWidth + 30.0f;
                        const f32 bank = glm::min(
                            view.height(p.x + dz / len * out, p.z - dx / len * out),
                            view.height(p.x - dz / len * out, p.z + dx / len * out));
                        f32 c = 0.0f;
                        const bool hasC = continent && continent->heightAt(p.x, p.z, c);
                        bool inLake = false;
                        for (const auto& lake : view.lakes) {
                            if (lake.covers(p.x, p.z)) {
                                inLake = true;
                                break;
                            }
                        }
                        char buf[96];
                        std::snprintf(buf, sizeof(buf), "[w%.1f g%+.0f c%+.0f b%+.0f hw%.0f%s] ",
                                      p.surface, view.height(p.x, p.z) - p.surface,
                                      hasC ? c - p.surface : 0.0f, bank - p.surface,
                                      p.halfWidth, inLake ? " L" : "");
                        zoom += buf;
                    }
                    LOG_INFO("    flat-in-gorge run of {} pts from ({:.0f}, {:.0f}), "
                             "every 24 m: {}",
                             bestB - bestA, river.points[bestA].x,
                             river.points[bestA].z, zoom);
                }
            }
        }
    }
    // Longitudinal: the water slope per ~48 m of course, length-weighted.
    f64 lenTotal = 0.0, lenFlat = 0.0, lenGentle = 0.0, lenSteep = 0.0;
    vector<f32> slopes;
    f64 dropTotal = 0.0;
    u32 courses = 0;
    for (const auto& river : view.rivers) {
        const size_t count = river.points.size();
        if (count < 8) {
            continue;
        }
        ++courses;
        dropTotal += river.points.front().surface - river.points.back().surface;
        for (size_t i = 0; i + 6 < count; i += 6) {
            f32 arc = 0.0f;
            for (size_t k = i + 1; k <= i + 6; ++k) {
                arc += std::hypot(river.points[k].x - river.points[k - 1].x,
                                  river.points[k].z - river.points[k - 1].z);
            }
            if (arc < 1.0f) {
                continue;
            }
            const f32 slope =
                (river.points[i].surface - river.points[i + 6].surface) / arc;
            slopes.push_back(100.0f * slope);
            lenTotal += arc;
            if (slope < 0.003f) {
                lenFlat += arc;
            } else if (slope < 0.02f) {
                lenGentle += arc;
            } else if (slope >= 0.08f) {
                lenSteep += arc;
            }
        }
    }
    if (lenTotal > 0.0) {
        LOG_INFO("  river slopes ({} courses, {:.1f} km, mean drop per "
                 "course {:.0f} m): flat < 0.3 % on {:.0f} % of the length, "
                 "gentle 0.3-2 % on {:.0f} %, steep >= 8 % on {:.0f} %; "
                 "slope median {:.2f} / p90 {:.2f} %",
                 courses, lenTotal / 1000.0,
                 courses ? dropTotal / courses : 0.0,
                 100.0 * lenFlat / lenTotal, 100.0 * lenGentle / lenTotal,
                 100.0 * lenSteep / lenTotal, median(slopes),
                 percentile(slopes, 90));
    }
    // Buried runs: consecutive course points whose published axis
    // ground stands > 2 m above the water (outside lakes), as RUNS.
    // Each run is sorted into one family by what surrounds it:
    //   groove  — a lower ground (< +2 m) within 60 m beside the axis:
    //             the polyline rides a spur next to its real channel;
    //   inherit — no groove and the continent ground is itself above
    //             the water there: the map routed water through
    //             terrain the continent never pierced;
    //   fine    — no groove, continent at or under the water: the fine
    //             stage (erosion, carve gate) raised the ground back.
    {
        constexpr f32 kBuried = 2.0f;
        constexpr f32 kGrooveOut = 60.0f;
        constexpr f32 kBorder = 100.0f;
        struct Run {
            f32 x, z, len, worst, groove, cont;
            u32 tier, family; // 0 groove, 1 inherit, 2 fine
            bool border;
        };
        vector<Run> runs;
        f64 courseLen = 0.0, buriedLen = 0.0;
        f64 lenByTier[3] = {}, buriedByTier[3] = {};
        for (const auto& river : view.rivers) {
            const size_t count = river.points.size();
            if (count < 3) {
                continue;
            }
            const u32 tier = glm::min(static_cast<u32>(river.tier), 2u);
            vector<f32> excess(count, 0.0f);
            vector<f32> seg(count, 0.0f);
            vector<u8> flag(count, 0);
            for (size_t i = 0; i < count; ++i) {
                const auto& pt = river.points[i];
                excess[i] = view.height(pt.x, pt.z) - pt.surface;
                flag[i] = excess[i] > kBuried && !underLake(pt.x, pt.z) ? 1 : 0;
                if (i > 0) {
                    seg[i] = std::hypot(pt.x - river.points[i - 1].x,
                                        pt.z - river.points[i - 1].z);
                    courseLen += seg[i];
                    lenByTier[tier] += seg[i];
                }
            }
            size_t i = 0;
            while (i < count) {
                if (!flag[i]) {
                    ++i;
                    continue;
                }
                size_t j = i;
                f32 len = 0.0f, worst = excess[i];
                size_t worstAt = i;
                while (j + 1 < count && flag[j + 1]) {
                    ++j;
                    len += seg[j];
                    if (excess[j] > worst) {
                        worst = excess[j];
                        worstAt = j;
                    }
                }
                buriedLen += len;
                buriedByTier[tier] += len;
                // Groove probe at the deepest burial, across the course.
                const auto& pt = river.points[worstAt];
                const size_t fwI = worstAt + 1 < count ? worstAt + 1 : worstAt;
                const size_t bwI = worstAt > 0 ? worstAt - 1 : worstAt;
                const f32 dx = river.points[fwI].x - river.points[bwI].x;
                const f32 dz = river.points[fwI].z - river.points[bwI].z;
                const f32 dl = glm::max(std::hypot(dx, dz), 0.01f);
                const f32 px = -dz / dl;
                const f32 pz = dx / dl;
                f32 groove = 1.0e30f;
                for (f32 d = 4.0f; d <= pt.halfWidth + kGrooveOut; d += 4.0f) {
                    groove = glm::min(groove,
                                      view.height(pt.x + px * d, pt.z + pz * d) -
                                          pt.surface);
                    groove = glm::min(groove,
                                      view.height(pt.x - px * d, pt.z - pz * d) -
                                          pt.surface);
                }
                f32 c = 0.0f;
                const bool hasC = continent && continent->heightAt(pt.x, pt.z, c);
                const f32 cont = hasC ? c - pt.surface : -1.0e30f;
                const u32 family = groove < kBuried ? 0u
                                   : cont > kBuried  ? 1u
                                                     : 2u;
                const bool border =
                    pt.x < view.minX + kBorder || pt.x > view.maxX - kBorder ||
                    pt.z < view.minZ + kBorder || pt.z > view.maxZ - kBorder;
                runs.push_back({ pt.x, pt.z, len, worst, groove, cont, tier,
                                 family, border });
                i = j + 1;
            }
        }
        if (!runs.empty() && courseLen > 0.0) {
            u32 byFamily[3] = {}, byFamilyBorder[3] = {};
            f64 lenByFamily[3] = {};
            vector<f32> lens, worsts;
            u32 shortRuns = 0, deepRuns = 0, shallowRuns = 0;
            for (const Run& r : runs) {
                ++byFamily[r.family];
                lenByFamily[r.family] += r.len;
                byFamilyBorder[r.family] += r.border ? 1 : 0;
                lens.push_back(r.len);
                worsts.push_back(r.worst);
                shortRuns += r.len <= 160.0f && r.worst <= 32.0f ? 1 : 0;
                shallowRuns += r.worst <= 8.0f ? 1 : 0;
                deepRuns += r.worst > 32.0f ? 1 : 0;
            }
            const auto pctLen = [](f64 part, f64 whole) {
                return whole > 0.0 ? 100.0 * part / whole : 0.0;
            };
            LOG_INFO("  buried runs (axis ground > {:.0f} m over the water, "
                     "outside lakes): {} runs, {:.1f} of {:.1f} km ({:.1f} %; by "
                     "tier r/R/F {:.1f}/{:.1f}/{:.1f} %), length median {:.0f} / "
                     "p90 {:.0f} m, burial median {:.1f} / p90 {:.1f} m; saddle-"
                     "sized (<= 160 m, <= 32 m) {}, shallow <= 8 m {}, deep > 32 "
                     "m {}",
                     kBuried, runs.size(), buriedLen / 1000.0, courseLen / 1000.0,
                     pctLen(buriedLen, courseLen),
                     pctLen(buriedByTier[0], lenByTier[0]),
                     pctLen(buriedByTier[1], lenByTier[1]),
                     pctLen(buriedByTier[2], lenByTier[2]), median(lens),
                     percentile(lens, 90), median(worsts),
                     percentile(worsts, 90), shortRuns, shallowRuns, deepRuns);
            LOG_INFO("  buried runs by family: groove beside (< {:.0f} m within "
                     "{:.0f} m) {} runs {:.1f} km ({} at a map line), continent "
                     "above the water too {} runs {:.1f} km ({} at a line), fine "
                     "stage only {} runs {:.1f} km ({} at a line)",
                     kBuried, kGrooveOut, byFamily[0], lenByFamily[0] / 1000.0,
                     byFamilyBorder[0], byFamily[1], lenByFamily[1] / 1000.0,
                     byFamilyBorder[1], byFamily[2], lenByFamily[2] / 1000.0,
                     byFamilyBorder[2]);
            std::sort(runs.begin(), runs.end(), [](const Run& a, const Run& b) {
                return a.len > b.len;
            });
            for (size_t n = 0; n < glm::min<size_t>(runs.size(), 8); ++n) {
                const Run& r = runs[n];
                LOG_INFO("    buried run tier {} at ({:.0f}, {:.0f}): {:.0f} m "
                         "long, burial {:.0f} m, groove beside {:+.0f} m, "
                         "continent {:+.0f} m, {}{}",
                         r.tier, r.x, r.z, r.len, r.worst,
                         r.groove < 1.0e29f ? r.groove : 0.0f,
                         r.cont > -1.0e29f ? r.cont : 0.0f,
                         r.family == 0 ? "groove" : r.family == 1 ? "inherit" : "fine",
                         r.border ? ", at a map line" : "");
            }
        }
    }
}

// The MASTER network's own courses (128 m, routed on the border-SHAPED
// surface): if a master course hugs the line, the imprint carves it
// there whatever the bake does afterwards. Then each fleuve course
// touching the map: where it rises, where it ends, whether the routing
// reached the sea or the super-cell rim.
void reportMasterCourses(const ReportContext& ctx) {
    const game::MapView& view = *ctx.view;
    const auto master = render::terraingen::masterRiversNear(
        ctx.controls, ctx.params.macro,
        ctx.net, view.minX - 1024.0f, view.minZ - 1024.0f,
        view.maxX + 1024.0f, view.maxZ + 1024.0f);
    u32 mTotal = 0, mNear = 0, mRun = 0;
    f32 mX = 0.0f, mZ = 0.0f;
    for (const auto& river : master) {
        u32 run = 0;
        for (const auto& node : river.nodes) {
            ++mTotal;
            if (mapLineDistance(view, node.x, node.z) < 150.0f) {
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
    // The master bed the imprint stamps (reprofileMasterCourse): how
    // much of it is flood-flat and how deep the bowls under it are —
    // the stage-1 side of the "rivers have no slope" question (the
    // imprint's bed floats over the bowls; the pierce drains them).
    {
        u32 nodes = 0, lakeNodes = 0, bowlNodes = 0;
        vector<f32> depths;
        f64 lenAll = 0.0, flatBed = 0.0, steepBed = 0.0;
        for (const auto& river : master) {
            if (river.nodes.size() < 3) {
                continue;
            }
            const auto bed = render::terraingen::reprofileMasterCourse(
                river, ctx.params.imprint, ctx.params.macro.seaLevel);
            for (size_t k = 0; k < river.nodes.size(); ++k) {
                ++nodes;
                lakeNodes += (river.nodes[k].flags &
                              render::terraingen::kMasterNodeLake) != 0
                                 ? 1
                                 : 0;
                bowlNodes += river.nodes[k].depth >
                                     ctx.params.imprint.bedDepth + 1.0f
                                 ? 1
                                 : 0;
                depths.push_back(river.nodes[k].depth);
                if (k + 1 < river.nodes.size()) {
                    const f32 run = glm::max(bed.arc[k + 1] - bed.arc[k], 1.0f);
                    const f32 sB = (bed.surface[k] - bed.surface[k + 1]) / run;
                    lenAll += run;
                    flatBed += sB < 0.003f ? run : 0.0f;
                    steepBed += sB >= 0.08f ? run : 0.0f;
                }
            }
        }
        const f64 L = glm::max(lenAll, 1.0);
        LOG_INFO("  master bed profiles: {} nodes ({} lake nodes, {} in a bowl "
                 "deeper than the bed; bowl depth median {:.0f} / p90 {:.0f} "
                 "/ max {:.0f} m), {:.1f} km; imprinted bed flat < 0.3 % on "
                 "{:.0f} %, steep >= 8 % on {:.0f} %",
                 nodes, lakeNodes, bowlNodes, median(depths),
                 percentile(depths, 90.0f),
                 depths.empty() ? 0.0f
                                : *std::max_element(depths.begin(), depths.end()),
                 lenAll / 1000.0, 100.0 * flatBed / L, 100.0 * steepBed / L);
    }
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

// The ground ACROSS the line at the longest run (32 m steps,
// -400..400 m): is the run in a trough, on a crest, at a rim? Then the
// same transect on the ANALYTIC and its components: which layer digs
// the trough.
void reportGroundAcrossLine(const ReportContext& ctx) {
    const game::MapView& view = *ctx.view;
    const auto& cp = ctx.cp;
    const auto& ctl = ctx.controls;
    const f32 longestX = ctx.longestX;
    const f32 longestZ = ctx.longestZ;
    const bool vertical =
        std::abs(longestX - view.minX) < 150.0f ||
        std::abs(longestX - view.maxX) < 150.0f;
    const f32 line = vertical
                         ? (std::abs(longestX - view.minX) < 150.0f
                                ? view.minX
                                : view.maxX)
                         : (std::abs(longestZ - view.minZ) < 150.0f
                                ? view.minZ
                                : view.maxZ);
    str profile;
    for (f32 d = -400.0f; d <= 400.0f; d += 32.0f) {
        const f32 x = vertical ? line + d : longestX;
        const f32 z = vertical ? longestZ : line + d;
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.0f ", view.height(x, z));
        profile += buf;
    }
    LOG_INFO("  ground across the line at the run ({} line {:.0f}, "
             "-400..+400 m by 32): {}",
             vertical ? "vertical" : "horizontal", line, profile);
    str an, base, plateau, basin, bed, storey;
    for (f32 d = -400.0f; d <= 400.0f; d += 32.0f) {
        const f32 x = vertical ? line + d : longestX;
        const f32 z = vertical ? longestZ : line + d;
        char buf[20];
        std::snprintf(buf, sizeof(buf), "%.0f ",
                      render::terraingen::macroHeightAnalytic(
                          ctl, ctx.params.macro, x, z));
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
    if (ctx.params.continent.enabled) {
        const auto& net = ctx.params.network;
        const auto continent = render::terraingen::loadContinentGround(
            ctx.cacheDir, ctx.params, view.mapSize,
            render::terraingen::continentSuperIndex(net, view.centreX()),
            render::terraingen::continentSuperIndex(net, view.centreZ()));
        str cont;
        for (f32 d = -400.0f; d <= 400.0f; d += 32.0f) {
            const f32 x = vertical ? line + d : longestX;
            const f32 z = vertical ? longestZ : line + d;
            f32 h = 0.0f;
            char buf[20];
            if (continent && continent->heightAt(x, z, h)) {
                std::snprintf(buf, sizeof(buf), "%.0f ", h);
            } else {
                std::snprintf(buf, sizeof(buf), "- ");
            }
            cont += buf;
        }
        LOG_INFO("  continent ground: {}", cont);
    }
    LOG_INFO("  base: {}", base);
    LOG_INFO("  plateau (POI lift): {}", plateau);
    LOG_INFO("  basinDepth: {}", basin);
    LOG_INFO("  bedDepth: {}", bed);
    LOG_INFO("  zone storey: {}", storey);
}

// The four border lines' resolved styles (a Sea arm IS a water channel
// along the line).
void reportBorderLines(const ReportContext& ctx) {
    const game::MapView& view = *ctx.view;
    const TileBakeParams& params = ctx.params;
    const auto& controls = ctx.controls;
    const i32 mapX = ctx.mapX;
    const i32 mapZ = ctx.mapZ;
    render::terraingen::MapGridSpec grid;
    grid.valid = true;
    grid.seed = params.worldSeed;
    grid.mapSize = view.mapSize;
    grid.seaLevel = params.macro.seaLevel;
    const auto styleName = [](render::terraingen::MapEdgeStyle st) {
        switch (st) {
        case render::terraingen::MapEdgeStyle::Sea: return "Sea";
        case render::terraingen::MapEdgeStyle::Ridges: return "Ridges";
        default: return "Open";
        }
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
void appendHistoryLine(const ReportContext& ctx) {
    const Census& c = ctx.census;
    std::ofstream log { ctx.cacheDir / "landscape-report.log",
                        std::ios::app };
    if (log) {
        const std::time_t now = std::time(nullptr);
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M",
                      std::localtime(&now));
        log << stamp << " map " << ctx.mapX << "," << ctx.mapZ << " key "
            << std::hex << ctx.key << std::dec << " | spawn slope "
            << c.meanSlopeSpawn << " % >30 " << c.steep30 << " % >45 "
            << c.steep45 << " % walls/km " << c.wallsPerKm << " relief250 "
            << c.relief250Spawn << " rise95 " << c.rise100p95 << " high "
            << c.highGroundSpawn << " % | map relief250 " << c.relief250Map
            << " high " << c.highGroundMap << " % lakes " << c.lakes
            << " rivers " << c.rivers << " | poi grand " << c.seeGrand
            << " % moyen " << c.seeMoyen << " % both " << c.seeBoth
            << " %\n";
    }
}

// The terrain mesh the game draws, at one LOD, at (x, z): the chunk
// mesher's vertices (multiples of `spacing`, meshHeight = the min over
// the flow-gated support at coarse LODs) and its triangulation (the
// i00-i11 diagonal). Vertices are memoised per LOD: a census touches
// each one many times.
struct LodMesh {
    const render::TerrainParams& tp;
    f32 spacing;
    std::unordered_map<u64, f32> vertices;

    f32 vertex(i64 ix, i64 iz) {
        const u64 key = (static_cast<u64>(static_cast<u32>(ix)) << 32) |
                        static_cast<u64>(static_cast<u32>(iz));
        const auto it = vertices.find(key);
        if (it != vertices.end()) {
            return it->second;
        }
        const f32 h = render::terrain::meshHeight(
            tp, static_cast<f32>(ix) * spacing, static_cast<f32>(iz) * spacing,
            spacing);
        vertices.emplace(key, h);
        return h;
    }
    f32 at(f32 x, f32 z) {
        const f32 fx = std::floor(x / spacing);
        const f32 fz = std::floor(z / spacing);
        const auto ix = static_cast<i64>(fx);
        const auto iz = static_cast<i64>(fz);
        const f32 tx = x / spacing - fx;
        const f32 tz = z / spacing - fz;
        const f32 h00 = vertex(ix, iz);
        const f32 h11 = vertex(ix + 1, iz + 1);
        if (tz >= tx) { // triangle i00, i01, i11
            const f32 h01 = vertex(ix, iz + 1);
            return h00 + (h11 - h01) * tx + (h01 - h00) * tz;
        }
        const f32 h10 = vertex(ix + 1, iz); // triangle i00, i11, i10
        return h00 + (h10 - h00) * tx + (h11 - h10) * tz;
    }
};

// The drawn mesh against the water, LOD by LOD: a coarse LOD bridges
// a bed narrower than its spacing shut and the ground then stands
// above the ribbon (the dev's "sol low poly au-dessus de la rivière").
// Same cuts as the river sections; per LOD the cuts whose axis the
// mesh covers and the share of the ribbon's width it covers.
void reportLodMeshVsWater(const ReportContext& ctx) {
    const game::MapView& view = *ctx.view;
    const auto underLake = [&](f32 x, f32 z) {
        for (const auto& lake : view.lakes) {
            if (lake.covers(x, z)) {
                return true;
            }
        }
        return false;
    };
    constexpr f32 kWetMargin = 0.2f;
    constexpr u32 kLods = render::TerrainSystem::kLodCount;
    vector<LodMesh> meshes;
    for (u32 lod = 0; lod < kLods; ++lod) {
        meshes.push_back({ view.tp,
                           render::TerrainSystem::kChunkSize /
                               static_cast<f32>(render::TerrainSystem::lodQuads(lod)) });
    }
    u32 cuts = 0;
    u32 axisOver[kLods] = {}, axisOverTier[kLods][3] = {};
    u32 byTier[3] = {};
    u64 ribbonN = 0, ribbonOver[kLods] = {};
    // Where the published ground itself stands over the water at the
    // axis: a carve residue (under 1 m), a terrace, or a buried stretch.
    u32 dryAxisBand[4] = {}; // < 1 m, 1-5 m, 5-20 m, > 20 m
    for (const auto& river : view.rivers) {
        const size_t count = river.points.size();
        if (count < 9) {
            continue;
        }
        for (size_t i = 3; i + 3 < count; i += 2) {
            const auto& pt = river.points[i];
            if (underLake(pt.x, pt.z) || pt.halfWidth < 0.5f ||
                !view.inside(pt.x, pt.z, 40.0f)) {
                continue;
            }
            const auto& fw = river.points[i + 1];
            const auto& bw = river.points[i - 1];
            const f32 dx = fw.x - bw.x;
            const f32 dz = fw.z - bw.z;
            const f32 len = std::sqrt(dx * dx + dz * dz);
            if (len < 0.5f) {
                continue;
            }
            const f32 px = -dz / len;
            const f32 pz = dx / len;
            const u32 tier = glm::min(pt.tier, 2u);
            ++cuts;
            ++byTier[tier];
            const i32 half = static_cast<i32>(pt.halfWidth);
            const f32 axisUp = view.height(pt.x, pt.z) - pt.surface;
            if (axisUp >= -kWetMargin) {
                ++dryAxisBand[axisUp < 1.0f ? 0 : axisUp < 5.0f ? 1
                                            : axisUp < 20.0f ? 2 : 3];
            }
            for (u32 lod = 0; lod < kLods; ++lod) {
                bool over = false;
                for (i32 k = -half; k <= half; ++k) {
                    const f32 d = static_cast<f32>(k);
                    const f32 m = meshes[lod].at(pt.x + px * d, pt.z + pz * d);
                    const bool above = m >= pt.surface - kWetMargin;
                    if (lod == 0) {
                        ++ribbonN;
                    }
                    ribbonOver[lod] += above ? 1 : 0;
                    over = over || (k == 0 && above);
                }
                if (over) {
                    ++axisOver[lod];
                    ++axisOverTier[lod][tier];
                }
            }
        }
    }
    if (cuts == 0) {
        return;
    }
    LOG_INFO("  lod mesh vs water: published ground over the water at the "
             "axis: < 1 m {}, 1-5 m {}, 5-20 m {}, > 20 m {} of {} cuts",
             dryAxisBand[0], dryAxisBand[1], dryAxisBand[2], dryAxisBand[3],
             cuts);
    for (u32 lod = 0; lod < kLods; ++lod) {
        LOG_INFO("  lod mesh vs water: LOD{} ({:.0f} m): mesh above the "
                 "water at the axis {} / {} cuts ({:.1f} %; r/R/F {}/{}/{} "
                 "of {}/{}/{}), {:.1f} % of the ribbon's width",
                 lod, meshes[lod].spacing, axisOver[lod], cuts,
                 100.0f * static_cast<f32>(axisOver[lod]) /
                     static_cast<f32>(cuts),
                 axisOverTier[lod][0], axisOverTier[lod][1],
                 axisOverTier[lod][2], byTier[0], byTier[1], byTier[2],
                 100.0 * static_cast<f64>(ribbonOver[lod]) /
                     static_cast<f64>(glm::max<u64>(ribbonN, 1)));
    }
    // Lake sheets: the floor is carved lakeDepthCoef m per m from the
    // shore, so near the rim it lies within a few dm of the sheet — a
    // coarse LOD drawn between a bank vertex and a rim vertex crosses
    // the sheet well inside the lake. Per LOD, the share of the lakes'
    // cells (their 8 m masks) the mesh covers.
    u64 lakeCells = 0, lakeOver[kLods] = {}, lakeDry = 0;
    for (const auto& lake : view.lakes) {
        if (lake.mask.empty()) {
            continue;
        }
        for (u32 mz = 0; mz < lake.maskHeight; ++mz) {
            for (u32 mx = 0; mx < lake.maskWidth; ++mx) {
                if (!lake.mask[static_cast<size_t>(mz) * lake.maskWidth + mx]) {
                    continue;
                }
                const f32 x = lake.minX + (static_cast<f32>(mx) + 0.5f) * lake.maskTexel;
                const f32 z = lake.minZ + (static_cast<f32>(mz) + 0.5f) * lake.maskTexel;
                if (!view.inside(x, z, 40.0f)) {
                    continue;
                }
                ++lakeCells;
                lakeDry += view.height(x, z) >= lake.level - kWetMargin ? 1 : 0;
                for (u32 lod = 0; lod < kLods; ++lod) {
                    lakeOver[lod] +=
                        meshes[lod].at(x, z) >= lake.level - kWetMargin ? 1 : 0;
                }
            }
        }
    }
    if (lakeCells > 0) {
        std::string line;
        for (u32 lod = 0; lod < kLods; ++lod) {
            char buf[48];
            std::snprintf(buf, sizeof(buf), " LOD%u %.1f %%", lod,
                          100.0 * static_cast<f64>(lakeOver[lod]) /
                              static_cast<f64>(lakeCells));
            line += buf;
        }
        LOG_INFO("  lod mesh vs water: lake cells ({} of {} lakes) with the "
                 "mesh above the sheet: ground {:.1f} %,{}",
                 lakeCells, view.lakes.size(),
                 100.0 * static_cast<f64>(lakeDry) / static_cast<f64>(lakeCells),
                 line);
    }
}

// The far water the game draws past the resident map (cached .twb
// of the neighbours, master fleuves beyond) against the ground the
// FarTerrain shows (sandboxFallbackHeight: the neighbours' overviews,
// then the continent): nodes hanging in the air or buried, with the
// master ribbons routed as they are and seated on that ground.
void reportFarWater(const ReportContext& ctx) {
    const game::MapView& view = *ctx.view;
    if (!view.sandbox) {
        return;
    }
    render::terraingen::ProceduralControlParams cp = ctx.params.controls;
    cp.seed = ctx.params.worldSeed;
    render::terraingen::MasterNetworkParams net = ctx.params.network;
    net.seaLevel = ctx.params.macro.seaLevel;
    net.mapGrid = view.sandbox->grid;
    constexpr f32 kPinHalfWidth = 4.0f; // the sim's pin (WaterSimControl)
    const auto census = [&](const render::SandboxTerrain* seatOn,
                            const char* label) {
        const auto set = game::collectFarWater(
            ctx.cacheDir, ctx.mapX, ctx.mapZ, ctx.params.tileSize, cp,
            ctx.params.macro, net, view.sandbox->grid,
            ctx.params.hydrology.widthLaw(), kPinHalfWidth,
            ctx.params.macro.seaLevel, view.centreX(), view.centreZ(),
            1.5f * view.mapSize, seatOn);
        vector<f32> master, baked;
        for (const auto& ribbon : set.ribbons) {
            for (const auto& n : ribbon.nodes) {
                const f32 d = n.surface -
                              render::sandboxFallbackHeight(*view.sandbox,
                                                            n.x, n.z);
                (ribbon.master ? master : baked).push_back(d);
            }
        }
        const auto line = [&](vector<f32>& v, const char* where) {
            if (v.empty()) {
                LOG_INFO("  far water ({}): {} none", label, where);
                return;
            }
            u32 air = 0, buried = 0;
            vector<f32> absd;
            for (const f32 d : v) {
                air += d > 2.0f ? 1 : 0;
                buried += d < -2.0f ? 1 : 0;
                absd.push_back(std::abs(d));
            }
            const f32 n = static_cast<f32>(v.size());
            LOG_INFO("  far water ({}): {} {} nodes: in the air > 2 m "
                     "{:.1f} %, buried < -2 m {:.1f} %, surface - horizon "
                     "ground median {:+.1f} / p90 {:+.1f} m, |d| p90 {:.1f} m",
                     label, where, v.size(), 100.0f * air / n,
                     100.0f * buried / n, median(v), percentile(v, 90),
                     percentile(absd, 90));
        };
        line(master, "master courses past the baked maps,");
        line(baked, "baked ribbons of this map and its neighbours,");
        LOG_INFO("  far water ({}): {} ribbons, {} lakes", label,
                 set.ribbons.size(), set.lakes.size());
    };
    census(nullptr, "routed");
    census(view.sandbox.get(), "seated");
}

// --at x z: the ribbons near one point, their ground, flow gate and
// drawn mesh at every LOD, and a transverse cut — for a spot the dev
// names in game.
void reportZoom(const ReportContext& ctx) {
    if (!ctx.zoomAt) {
        return;
    }
    const game::MapView& view = *ctx.view;
    const Vec2 at = *ctx.zoomAt;
    constexpr u32 kLods = render::TerrainSystem::kLodCount;
    vector<LodMesh> meshes;
    for (u32 lod = 0; lod < kLods; ++lod) {
        meshes.push_back({ view.tp,
                           render::TerrainSystem::kChunkSize /
                               static_cast<f32>(render::TerrainSystem::lodQuads(lod)) });
    }
    const auto flowAt = [&](f32 x, f32 z) {
        const render::TerrainRegion* r =
            view.tp.base ? view.tp.base->regionAt(x, z) : nullptr;
        return r ? render::terrain::maskSample(*r, r->flow, x, z, 0.0f) : 0.0f;
    };
    LOG_INFO("  zoom at ({:.0f}, {:.0f}): ground {:.1f} m, flow {:.2f}, "
             "wet {}",
             at.x, at.y, view.height(at.x, at.y), flowAt(at.x, at.y),
             view.wet(at.x, at.y) ? "yes" : "no");
    for (size_t l = 0; l < view.lakes.size(); ++l) {
        const auto& lake = view.lakes[l];
        if (!lake.covers(at.x, at.y)) {
            continue;
        }
        std::string lods;
        for (u32 lod = 0; lod < kLods; ++lod) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), " LOD%u %+.2f", lod,
                          meshes[lod].at(at.x, at.y) - lake.level);
            lods += buf;
        }
        LOG_INFO("    lake {} level {:.2f} m, {} cells, {} [{:.0f}..{:.0f}] x "
                 "[{:.0f}..{:.0f}]: ground - level {:+.2f}; mesh - level at "
                 "the point:{}",
                 l, lake.level, lake.cells, lake.dug ? "pond" : "natural",
                 lake.minX, lake.maxX, lake.minZ, lake.maxZ,
                 view.height(at.x, at.y) - lake.level, lods);
        // The lake's own census: floor and meshes against the sheet.
        u64 n = 0, dry = 0, over[kLods] = {};
        for (u32 mz = 0; mz < lake.maskHeight; ++mz) {
            for (u32 mx = 0; mx < lake.maskWidth; ++mx) {
                if (!lake.mask[static_cast<size_t>(mz) * lake.maskWidth + mx]) {
                    continue;
                }
                const f32 x = lake.minX + (static_cast<f32>(mx) + 0.5f) * lake.maskTexel;
                const f32 z = lake.minZ + (static_cast<f32>(mz) + 0.5f) * lake.maskTexel;
                ++n;
                dry += view.height(x, z) >= lake.level - 0.2f ? 1 : 0;
                for (u32 lod = 0; lod < kLods; ++lod) {
                    over[lod] += meshes[lod].at(x, z) >= lake.level - 0.2f ? 1 : 0;
                }
            }
        }
        std::string line;
        for (u32 lod = 0; lod < kLods; ++lod) {
            char buf[48];
            std::snprintf(buf, sizeof(buf), " LOD%u %llu", lod,
                          static_cast<unsigned long long>(over[lod]));
            line += buf;
        }
        LOG_INFO("    lake {}: {} mask cells, ground at/over the sheet {}, "
                 "mesh over the sheet:{}",
                 l, n, dry, line);
        // Ground and the LOD4 mesh against the sheet on a +-48 m grid
        // (8 m), rows along +z.
        for (i32 dz = -48; dz <= 48; dz += 8) {
            std::string g, m;
            for (i32 dx = -48; dx <= 48; dx += 8) {
                const f32 x = at.x + static_cast<f32>(dx);
                const f32 z = at.y + static_cast<f32>(dz);
                char buf[16];
                const bool in = lake.covers(x, z);
                std::snprintf(buf, sizeof(buf), "%s%+.1f ", in ? "" : "(",
                              view.height(x, z) - lake.level);
                g += buf;
                std::snprintf(buf, sizeof(buf), "%s%+.1f ", in ? "" : "(",
                              meshes[4].at(x, z) - lake.level);
                m += buf;
            }
            LOG_INFO("      z{:+d}: ground {} | LOD4 {}", dz, g, m);
        }
    }
    struct Near {
        f32 d2;
        size_t river, i;
    };
    vector<Near> near;
    for (size_t r = 0; r < view.rivers.size(); ++r) {
        const auto& pts = view.rivers[r].points;
        for (size_t i = 1; i + 1 < pts.size(); ++i) {
            const f32 d2 = (pts[i].x - at.x) * (pts[i].x - at.x) +
                           (pts[i].z - at.y) * (pts[i].z - at.y);
            if (d2 < 150.0f * 150.0f) {
                near.push_back({ d2, r, i });
            }
        }
    }
    std::sort(near.begin(), near.end(),
              [](const Near& a, const Near& b) { return a.d2 < b.d2; });
    size_t shown = 0;
    size_t lastRiver = SIZE_MAX;
    for (const Near& n : near) {
        if (n.river == lastRiver || shown >= 3) {
            continue; // one point per course: the nearest
        }
        lastRiver = n.river;
        ++shown;
        const auto& river = view.rivers[n.river];
        const auto& pt = river.points[n.i];
        std::string lods;
        for (u32 lod = 0; lod < kLods; ++lod) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), " LOD%u %+.1f", lod,
                          meshes[lod].at(pt.x, pt.z) - pt.surface);
            lods += buf;
        }
        LOG_INFO("    river {} tier {} pt {}/{} at ({:.0f}, {:.0f}) {:.0f} m "
                 "away: water {:.1f}, hw {:.1f}, ground - water {:+.1f}, "
                 "flow {:.2f}; mesh - water at the axis:{}",
                 n.river, river.tier, n.i, river.points.size(), pt.x, pt.z,
                 std::sqrt(n.d2), pt.surface, pt.halfWidth,
                 view.height(pt.x, pt.z) - pt.surface, flowAt(pt.x, pt.z),
                 lods);
        const auto& fw = river.points[n.i + 1];
        const auto& bw = river.points[n.i - 1];
        const f32 dx = fw.x - bw.x;
        const f32 dz = fw.z - bw.z;
        const f32 len = glm::max(std::sqrt(dx * dx + dz * dz), 0.01f);
        const f32 px = -dz / len;
        const f32 pz = dx / len;
        const i32 reach = static_cast<i32>(glm::max(2.0f * pt.halfWidth, 24.0f));
        const i32 step = reach > 40 ? 4 : 2;
        std::string ground, m2, m4;
        for (i32 d = -reach; d <= reach; d += step) {
            const f32 x = pt.x + px * static_cast<f32>(d);
            const f32 z = pt.z + pz * static_cast<f32>(d);
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%+.0f ", view.height(x, z) - pt.surface);
            ground += buf;
            std::snprintf(buf, sizeof(buf), "%+.0f ", meshes[2].at(x, z) - pt.surface);
            m2 += buf;
            std::snprintf(buf, sizeof(buf), "%+.0f ", meshes[4].at(x, z) - pt.surface);
            m4 += buf;
        }
        LOG_INFO("      cut -{}..+{} m by {}, ground - water: {}", reach, reach,
                 step, ground);
        LOG_INFO("      LOD2 (4 m) mesh - water:         {}", m2);
        LOG_INFO("      LOD4 (16 m) mesh - water:        {}", m4);
        // The course around the point: one line per published point
        // over +-20 points — the water, the half-width, the ground
        // under the axis and the lowest ground across the ribbon's
        // core, the slope to the next point, and the lake covering
        // it if any. Reads an interrupted ribbon or a dry sim reach
        // off the bake (a run's end, a lake cut, a core proud of the
        // water, a step).
        {
            const size_t lo = n.i >= 20 ? n.i - 20 : 0;
            const size_t hi = glm::min(n.i + 20, river.points.size() - 1);
            LOG_INFO("      course {} pts {}..{} (head {}, {} pts, "
                     "lake-fed {}): i | x z | water | hw | axis-water | "
                     "core min-water | slope % | lake", n.river, lo, hi,
                     river.headIsSource ? "source" : "outlet",
                     river.points.size(), river.lakeFed ? "yes" : "no");
            for (size_t k = lo; k <= hi; ++k) {
                const auto& p = river.points[k];
                const auto& f = river.points[glm::min(k + 1, river.points.size() - 1)];
                const auto& b = river.points[k > 0 ? k - 1 : 0];
                const f32 ddx = f.x - b.x;
                const f32 ddz = f.z - b.z;
                const f32 dl = glm::max(std::sqrt(ddx * ddx + ddz * ddz), 0.01f);
                const f32 nx = -ddz / dl;
                const f32 nz = ddx / dl;
                f32 coreMin = 1.0e30f;
                const f32 core = glm::max(p.halfWidth - 2.0f, 0.0f);
                for (i32 s = -4; s <= 4; ++s) {
                    const f32 d = core * static_cast<f32>(s) / 4.0f;
                    coreMin = glm::min(coreMin,
                                       view.height(p.x + nx * d, p.z + nz * d));
                }
                const f32 seg = std::hypot(f.x - p.x, f.z - p.z);
                const f32 slope = seg > 0.01f ? 100.0f * (p.surface - f.surface) / seg : 0.0f;
                i32 lakeIdx = -1;
                for (size_t l = 0; l < view.lakes.size(); ++l) {
                    if (view.lakes[l].covers(p.x, p.z)) {
                        lakeIdx = static_cast<i32>(l);
                        break;
                    }
                }
                LOG_INFO("        {:>4} | {:7.0f} {:7.0f} | {:7.1f} | {:5.1f} | "
                         "{:+6.1f} | {:+6.1f} | {:+6.1f} | {}",
                         k, p.x, p.z, p.surface, p.halfWidth,
                         view.height(p.x, p.z) - p.surface, coreMin - p.surface,
                         slope, lakeIdx >= 0 ? std::to_string(lakeIdx) : "-");
            }
        }
        // Every published point of ANY course within 40 m: a
        // tributary mouth or a sibling run hanging over the spot
        // (the nearest-course table above cannot show it).
        LOG_INFO("      all course points within 40 m: course i | x z | "
                 "water | hw | tier | ground - water");
        for (size_t r = 0; r < view.rivers.size(); ++r) {
            const auto& other = view.rivers[r];
            for (size_t k = 0; k < other.points.size(); ++k) {
                const auto& p = other.points[k];
                const f32 dx = p.x - at.x;
                const f32 dz = p.z - at.y;
                if (dx * dx + dz * dz > 40.0f * 40.0f) {
                    continue;
                }
                LOG_INFO("        {:>4} {:>4} | {:7.0f} {:7.0f} | {:7.1f} | "
                         "{:5.1f} | {} | {:+6.1f}",
                         r, k, p.x, p.z, p.surface, p.halfWidth, p.tier,
                         view.height(p.x, p.z) - p.surface);
            }
        }
    }
    if (shown == 0) {
        LOG_INFO("    no course within 150 m");
    }
}

} // namespace

int landscapeReport(char** argv, int argc) {
    const std::filesystem::path gameDir = argv[2];
    const i32 mapX = argc >= 5 ? std::atoi(argv[3]) : 0;
    const i32 mapZ = argc >= 5 ? std::atoi(argv[4]) : 0;
    std::optional<Vec2> zoomAt;
    if (argc == 8 && std::string(argv[5]) == "--at") {
        zoomAt = Vec2 { static_cast<f32>(std::atof(argv[6])),
                        static_cast<f32>(std::atof(argv[7])) };
    }

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

    ReportContext ctx { params, mapX, mapZ, tuning.terrainSeed, cacheDir };
    ctx.zoomAt = zoomAt;
    reportColdCosts(ctx);
    if (!bakeIfStale(ctx) || !loadBakedMap(ctx)) {
        return 1;
    }
    renderPlan(ctx);
    renderRelief(ctx);
    reportAnalyticCosts(ctx);
    reportCensus(ctx);
    reportLakes(ctx);
    reportOrphanTails(ctx);
    reportContinent(ctx);
    reportLakesVsZones(ctx);
    reportWallsAndGates(ctx);
    reportPoiVisibility(ctx);
    reportRiversVsMapLines(ctx);
    reportRiverSections(ctx);
    reportLodMeshVsWater(ctx);
    reportFarWater(ctx);
    reportZoom(ctx);
    reportMasterCourses(ctx);
    reportGroundAcrossLine(ctx);
    reportBorderLines(ctx);
    appendHistoryLine(ctx);
    return 0;
}

} // namespace cooker
