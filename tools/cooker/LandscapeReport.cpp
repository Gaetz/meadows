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

#include <stb_image_write.h>

#include "data/forms/LandscapeForms.hpp"
#include "data/plugins/PluginConfig.hpp"
#include "data/plugins/Resolver.hpp"
#include "engine/core/Jobs.hpp"
#include "engine/core/Log.hpp"
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

    ReportContext ctx { params, mapX, mapZ, tuning.terrainSeed, cacheDir };
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
    reportMasterCourses(ctx);
    reportGroundAcrossLine(ctx);
    reportBorderLines(ctx);
    appendHistoryLine(ctx);
    return 0;
}

} // namespace cooker
