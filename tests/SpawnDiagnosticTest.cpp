#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <map>

#include "MapWorldFixture.hpp"
#include "engine/assets/GltfMesh.hpp"
#include "engine/assets/MeshSimplify.hpp"
#include "engine/terrain/generation/MasterNetwork.hpp"

// Hidden landscape INSTRUMENTS (every case is doctest::skip — run one
// explicitly, e.g. meadows-tests '-tc=variety transect*' -ns): the
// headless twins of "walk the world and look" that measure the target
// of docs/PAYSAGE.md §4 on THE BOUNDED MAP the game plays — map (0, 0)
// of seed 1337, read from a map cache through MapWorldFixture (global
// erosion, border ranges, one hydrology; the spawn probed like the
// game does). Cache: MEADOWS_MAP_CACHE=<terrain-cache/<seed>> when set,
// else the game's cache next to the binary, else a temp bake (minutes
// in Debug — point MEADOWS_MAP_CACHE at a baked one). Transects,
// vistas, family/calm/snow census, lakes, rivers, fleuves, erosion
// calibration, plus asset QA (rock uv). The numbers they print are the
// baseline of docs/PAYSAGE.md §7.4.

using namespace render::terraingen;

namespace {

constexpr u32 kSeed = 1337;
constexpr i32 kMapX = 0;
constexpr i32 kMapZ = 0;
// Past the rim ranges: where the census of the INTERIOR runs.
constexpr f32 kInterior = kMapBorderMountainHalf;

const maptest::MapWorld& theMap() {
    static const maptest::MapWorld world = [] {
        const TileBakeParams params = maptest::gameLikeParams(kSeed);
        const auto root =
            maptest::diagnosticsCacheRoot(params, kMapX, kMapZ);
        MESSAGE("map (", kMapX, ", ", kMapZ, ") from ", root.string());
        maptest::MapWorld w = maptest::loadOrBakeMap(
            params, kMapX, kMapZ, game::kMapTilesPerSide, root);
        MESSAGE("  ", w.slicesLoaded, " slices, ", w.lakes.size(),
                " lakes, ", w.rivers.size(), " river runs, overview ",
                w.sandbox->overviewGrid.n, "^2");
        return w;
    }();
    return world;
}

ProceduralControls controlsOf(const maptest::MapWorld& w) {
    return ProceduralControls { w.controlParams };
}

// The game's start on this map (shared probe), the centre otherwise.
Vec3 spawnOf(const maptest::MapWorld& w) {
    if (const auto spot = render::probeMapSpawn(*w.sandbox, w.mapX, w.mapZ,
                                                w.seaLevel())) {
        return *spot;
    }
    return { w.centreX(), w.overviewHeight(w.centreX(), w.centreZ()),
             w.centreZ() };
}

// The analytic mirror with the border shaping — what the bake started
// from (and what the far fallback shows where no overview exists).
f32 analyticHeight(const maptest::MapWorld& w, const ProceduralControls& c,
                   f32 x, f32 z) {
    return applyMapGridShape(c, w.params.macro, w.sandbox->grid, x, z,
                             macroHeightAnalytic(c, w.params.macro, x, z));
}

} // namespace

// Land-type budget of the map: how its land (sea excluded) splits into
// plains / hills+plateaus / mountains, sampled from the control fields.
//   meadows-tests '-tc=proportion diagnostic' -ns
TEST_CASE("proportion diagnostic" * doctest::skip()) {
    const maptest::MapWorld& w = theMap();
    const ProceduralControls controls = controlsOf(w);
    u64 sea = 0, plains = 0, hills = 0, mountains = 0;
    for (f32 z = w.minZ; z <= w.maxZ; z += 200.0f) {
        for (f32 x = w.minX; x <= w.maxX; x += 200.0f) {
            const ControlSample s = controls.at(x, z);
            if (s.sea) {
                ++sea;
            } else if (s.uplift > 0.35f) {
                ++mountains;
            } else if (s.hillRelief > 25.0f || s.plateau > 80.0f) {
                ++hills;
            } else {
                ++plains;
            }
        }
    }
    const f64 land = static_cast<f64>(plains + hills + mountains);
    MESSAGE("sea ", 100.0 * sea / (land + sea), "% of the map; of land: ",
            "plains ", 100.0 * plains / land, "%, hills+plateaus ",
            100.0 * hills / land, "%, mountains ",
            100.0 * mountains / land, "%");
    CHECK(land > 0.0);
}

// Coastline census of the map: how much of the shore runs in cliff
// mode, how high the rims stand, where the best sea-cliffs are — and
// whether the rim survives the real (global) erosion.
//   meadows-tests '-tc=coast diagnostic' -ns
TEST_CASE("coast diagnostic" * doctest::skip()) {
    const maptest::MapWorld& w = theMap();
    const ProceduralControls controls = controlsOf(w);
    const f32 sea = w.seaLevel();
    struct Spot {
        f32 x, z, rim, cliff;
    };
    vector<Spot> shore;
    const f32 step = 200.0f;
    for (f32 z = w.minZ; z <= w.maxZ; z += step) {
        for (f32 x = w.minX; x <= w.maxX; x += step) {
            const ControlSample s = controls.at(x, z);
            if (s.sea) {
                continue;
            }
            const bool coastal = controls.at(x - step, z).sea ||
                                 controls.at(x + step, z).sea ||
                                 controls.at(x, z - step).sea ||
                                 controls.at(x, z + step).sea;
            if (!coastal) {
                continue;
            }
            const f32 cliff = glm::max(
                glm::smoothstep(w.params.macro.cliffTierStart,
                                w.params.macro.cliffTierEnd, s.tier),
                glm::smoothstep(0.62f, 0.8f, s.hardness));
            const f32 rim = analyticHeight(w, controls, x, z) - sea;
            shore.push_back({ x, z, rim, cliff });
        }
    }
    u32 cliffy = 0, tall = 0;
    for (const Spot& s : shore) {
        if (s.cliff > 0.5f) {
            ++cliffy;
            if (s.rim > 40.0f) {
                ++tall;
            }
        }
    }
    MESSAGE("shore samples: ", shore.size(), "; cliff-mode ",
            shore.empty() ? 0.0 : 100.0 * cliffy / shore.size(),
            "%, of which rim>40m ", cliffy ? 100.0 * tall / cliffy : 0.0,
            "%");
    std::sort(shore.begin(), shore.end(),
              [](const Spot& a, const Spot& b) {
                  return a.rim * a.cliff > b.rim * b.cliff;
              });
    vector<Spot> kept;
    for (const Spot& s : shore) {
        bool near = false;
        for (const Spot& other : kept) {
            if (std::hypot(s.x - other.x, s.z - other.z) < 5000.0f) {
                near = true;
                break;
            }
        }
        if (near || s.cliff < 0.5f) {
            continue;
        }
        kept.push_back(s);
        MESSAGE("cliff coast at (", s.x, ", ", s.z, "): rim ", s.rim,
                " m, cliff ", s.cliff);
        if (kept.size() >= 5) {
            break;
        }
    }
    // Walk a transect through the top spot on the PUBLISHED ground:
    // does the rim survive the erosion?
    if (!kept.empty()) {
        const Spot& top = kept.front();
        f32 dx = 0.0f, dz = 0.0f;
        if (controls.at(top.x - step, top.z).sea) {
            dx = -1.0f;
        } else if (controls.at(top.x + step, top.z).sea) {
            dx = 1.0f;
        } else if (controls.at(top.x, top.z - step).sea) {
            dz = -1.0f;
        } else {
            dz = 1.0f;
        }
        for (f32 d = -600.0f; d <= 600.0f; d += 150.0f) {
            MESSAGE("  transect ", d, " m seaward: baked h = ",
                    w.height(top.x + dx * d, top.z + dz * d));
        }
    }
    CHECK(true);
}

// Calibration data for the erosion-aware analytic (far silhouettes):
// buckets published-minus-analytic over the whole map by analytic
// height-above-sea, with the mean keep fraction — the re-fit
// instrument of the analytic mirror.
//   meadows-tests '-tc=erosion calibration' -ns
TEST_CASE("erosion calibration" * doctest::skip()) {
    const maptest::MapWorld& w = theMap();
    const ProceduralControls controls = controlsOf(w);
    const f32 sea = w.seaLevel();
    struct Bucket {
        f64 delta { 0.0 };
        f64 keep { 0.0 };
        u32 count { 0 };
    };
    constexpr u32 kBuckets = 16;
    constexpr f32 kBand = 100.0f;
    array<Bucket, kBuckets> buckets {};
    for (f32 z = w.minZ + kInterior; z <= w.maxZ - kInterior; z += 48.0f) {
        for (f32 x = w.minX + kInterior; x <= w.maxX - kInterior;
             x += 48.0f) {
            const f32 ha = analyticHeight(w, controls, x, z);
            if (ha <= sea) {
                continue;
            }
            const u32 b =
                glm::min(kBuckets - 1, static_cast<u32>((ha - sea) / kBand));
            const f32 hb = w.height(x, z);
            const ControlSample s = controls.at(x, z);
            buckets[b].delta += hb - ha;
            buckets[b].keep += glm::min(0.5f, s.plateau * 0.0008f);
            ++buckets[b].count;
        }
    }
    for (u32 b = 0; b < kBuckets; ++b) {
        if (buckets[b].count < 8) {
            continue;
        }
        MESSAGE("  h-sea [", b * 100, ",", (b + 1) * 100, "): mean delta ",
                buckets[b].delta / buckets[b].count, " keep ",
                buckets[b].keep / buckets[b].count, " (n=",
                buckets[b].count, ")");
    }
    CHECK(true);
}

// How much relief does each erosion stage take? Bakes the map's
// tallest slice ALONE (a 1x1 map, no borders) with stages toggled off
// and reports the height stats — the answer to "does erosion flatten
// everything".
//   meadows-tests '-tc=erosion strength*' -ns
TEST_CASE("erosion strength diagnostic" * doctest::skip()) {
    const maptest::MapWorld& w = theMap();
    // The tallest slice of the published map.
    i32 bestTx = w.mapX * w.tilesPerSide;
    i32 bestTz = w.mapZ * w.tilesPerSide;
    f32 bestH = -1.0e9f;
    for (const auto& region : w.tp.base->regions) {
        f32 maxH = -1.0e9f;
        for (size_t i = 0; i < region->heights.size(); i += 7) {
            maxH = glm::max(maxH, region->heights[i]);
        }
        if (maxH > bestH) {
            bestH = maxH;
            bestTx = static_cast<i32>(std::floor(
                (region->originX + 100.0f) / w.params.tileSize));
            bestTz = static_cast<i32>(std::floor(
                (region->originZ + 100.0f) / w.params.tileSize));
        }
    }
    MESSAGE("tallest slice (", bestTx, ", ", bestTz, "): ", bestH, " m");
    const auto stats = [&](const char* label, TileBakeParams params) {
        params.mapGrid.valid = false;
        const TileBakeResult r = bakeSoloTile(params, bestTx, bestTz);
        vector<f32> above;
        const f32 sea = params.macro.seaLevel;
        f32 maxH = 0.0f;
        for (u32 row = 0; row < r.region.height; row += 4) {
            for (u32 col = 0; col < r.region.width; col += 4) {
                const f32 h =
                    r.region.heights[static_cast<size_t>(row) *
                                         r.region.width +
                                     col];
                maxH = glm::max(maxH, h);
                if (h > sea) {
                    above.push_back(h - sea);
                }
            }
        }
        std::sort(above.begin(), above.end());
        const auto pct = [&](f32 p) {
            return above.empty()
                       ? 0.0f
                       : above[static_cast<size_t>(
                             p * static_cast<f32>(above.size() - 1))];
        };
        f64 mean = 0.0;
        for (const f32 h : above) {
            mean += h;
        }
        mean /= glm::max<size_t>(above.size(), 1);
        MESSAGE(std::string(label), ": max=", maxH, " mean-above-sea=", mean);
        MESSAGE("  p10=", pct(0.10f), " p25=", pct(0.25f),
                " p40=", pct(0.40f), " p50=", pct(0.50f),
                " p60=", pct(0.60f), " p75=", pct(0.75f),
                " p90=", pct(0.90f), " p95=", pct(0.95f),
                " p99=", pct(0.99f));
    };
    const TileBakeParams base = w.params;
    stats("default            ", base);
    TileBakeParams noRound = base;
    noRound.rounding.strength = 0.0f;
    stats("rounding OFF       ", noRound);
    TileBakeParams noErosion = noRound;
    noErosion.fluvial.iterations = 0;
    noErosion.thermal.iterations = 0;
    stats("erosion+rounding OFF", noErosion);
    for (const i32 iterations : { 60, 40 }) {
        TileBakeParams softer = base;
        softer.fluvial.iterations = iterations;
        stats("fluvial reduced     ", softer);
    }
    CHECK(true);
}

// Variety along a walk: two 16 km transects through the map's spawn
// (E-W and N-S) on the published ground, scored by 250 m windows —
// the distance a player runs in ~45 s (5.5 m/s). A window is an
// "event" when the relief regime flips, water is crossed, or local
// relief exceeds 25 m. Samples in the rim ranges (and off the map) are
// skipped: the census is of the interior.
//   meadows-tests '-tc=variety transect*' -ns
TEST_CASE("variety transect diagnostic" * doctest::skip()) {
    const maptest::MapWorld& w = theMap();
    const ProceduralControls controls = controlsOf(w);
    const f32 seaLevel = w.seaLevel();
    const Vec3 spawn = spawnOf(w);
    const f32 px = spawn.x;
    const f32 pz = spawn.z;
    MESSAGE("spawn (", px, ", ", spawn.y, ", ", pz, ")");
    const f32 kHalf = 8000.0f;
    const f32 kStep = 25.0f;

    const auto wetAt = [&](f32 x, f32 z, f32 h) {
        if (h < seaLevel + 0.5f) {
            return true;
        }
        if (render::terrain::waterSurfaceAt(w.bodies, x, z, h + 1.0f)
                .has_value()) {
            return true;
        }
        for (const River& river : w.rivers) {
            for (const RiverPoint& p : river.points) {
                const f32 reach = glm::max(p.halfWidth, 4.0f);
                if (std::abs(p.x - x) < reach && std::abs(p.z - z) < reach) {
                    return true;
                }
            }
        }
        return false;
    };
    const auto regimeOf = [&](f32 x, f32 z) -> int {
        const ControlSample s = controls.at(x, z);
        if (s.sea) {
            return 0;
        }
        if (s.uplift > 0.35f) {
            return 3;
        }
        if (s.hillRelief > 25.0f || s.plateau > 80.0f) {
            return 2;
        }
        return 1;
    };
    const auto runTransect = [&](const char* label, f32 dirX, f32 dirZ) {
        constexpr u32 kWindow = 10; // 10 x 25 m = 250 m = ~45 s of run
        constexpr f32 kTan10 = 0.1763f;
        constexpr f32 kTan15 = 0.2679f;
        constexpr f32 kTan30 = 0.5774f;
        f32 minH = 1.0e9f, maxH = -1.0e9f;
        f64 meanH = 0.0;
        u32 samples = 0, steep15 = 0, steep30 = 0, skipped = 0;
        vector<f32> windowRelief;
        u32 flatWindows = 0, reliefEvents = 0, regimeEvents = 0,
            waterEvents = 0;
        u32 socleWindows = 0, versantWindows = 0, drameWindows = 0;
        u32 plateauWindows = 0;
        u32 seaWindows = 0;
        f32 maxImpassable = 0.0f, sinceFoothold = 0.0f;
        bool wallInRun = false;
        vector<u8> eventTypes; // 0 relief, 1 regime, 2 water
        f32 lastEventD = -kHalf;
        f32 worstGap = 0.0f;
        f64 gapSum = 0.0;
        u32 gapCount = 0;
        int prevRegime = -1;
        bool prevWet = false;
        f32 wMin = 1.0e9f, wMax = -1.0e9f;
        u32 inWindow = 0;
        f32 prevH = 0.0f;
        bool havePrev = false;
        vector<f32> windowSlopes;
        for (f32 d = -kHalf; d <= kHalf; d += kStep) {
            const f32 x = px + dirX * d;
            const f32 z = pz + dirZ * d;
            if (!w.inside(x, z, kInterior)) {
                ++skipped;
                havePrev = false;
                continue;
            }
            const f32 h = w.height(x, z);
            minH = glm::min(minH, h);
            maxH = glm::max(maxH, h);
            meanH += h;
            if (havePrev) {
                const f32 slope = std::abs(h - prevH) / kStep;
                if (slope > kTan30) {
                    ++steep30;
                } else if (slope > kTan15) {
                    ++steep15;
                }
                windowSlopes.push_back(slope);
                if (slope < kTan15) {
                    if (wallInRun) {
                        maxImpassable =
                            glm::max(maxImpassable, sinceFoothold);
                    }
                    sinceFoothold = 0.0f;
                    wallInRun = false;
                } else {
                    sinceFoothold += kStep;
                    wallInRun = wallInRun || slope > kTan30;
                }
            }
            prevH = h;
            havePrev = true;
            ++samples;
            wMin = glm::min(wMin, h);
            wMax = glm::max(wMax, h);
            if (++inWindow < kWindow) {
                continue;
            }
            // One 250 m window closes here. Fully open-water windows
            // leave the census (they would read as flat socle) and
            // pause the event clock: crossing a gulf is its own
            // continuous experience, not landscape monotony.
            if (wMax < seaLevel + 0.5f) {
                ++seaWindows;
                lastEventD = d;
                prevRegime = -1;
                prevWet = true;
                windowSlopes.clear();
                wMin = 1.0e9f;
                wMax = -1.0e9f;
                inWindow = 0;
                continue;
            }
            const f32 relief = wMax - wMin;
            windowRelief.push_back(relief);
            std::sort(windowSlopes.begin(), windowSlopes.end());
            const f32 medianSlope =
                windowSlopes.empty() ? 0.0f
                                     : windowSlopes[windowSlopes.size() / 2];
            windowSlopes.clear();
            const f32 cx = px + dirX * (d - 125.0f);
            const f32 cz = pz + dirZ * (d - 125.0f);
            if (medianSlope > kTan30) {
                ++drameWindows;
            } else if (medianSlope < kTan10 && relief < 15.0f) {
                ++socleWindows;
                if (controls.at(cx, cz).plateau > 80.0f) {
                    ++plateauWindows;
                }
            } else {
                ++versantWindows;
            }
            const int regime = regimeOf(cx, cz);
            const bool wet = wetAt(cx, cz, w.height(cx, cz));
            bool event = false;
            if (relief < 8.0f) {
                ++flatWindows;
            }
            if (relief > 25.0f) {
                ++reliefEvents;
                eventTypes.push_back(0);
                event = true;
            }
            if (prevRegime >= 0 && regime != prevRegime) {
                ++regimeEvents;
                eventTypes.push_back(1);
                event = true;
            }
            if (wet && !prevWet) {
                ++waterEvents;
                eventTypes.push_back(2);
                event = true;
            }
            prevRegime = regime;
            prevWet = wet;
            if (event) {
                const f32 gap = d - lastEventD;
                worstGap = glm::max(worstGap, gap);
                gapSum += gap;
                ++gapCount;
                lastEventD = d;
            }
            wMin = 1.0e9f;
            wMax = -1.0e9f;
            inWindow = 0;
        }
        worstGap = glm::max(worstGap, kHalf - lastEventD);
        if (wallInRun) {
            maxImpassable = glm::max(maxImpassable, sinceFoothold);
        }
        std::sort(windowRelief.begin(), windowRelief.end());
        const f32 medianRelief =
            windowRelief.empty() ? 0.0f
                                 : windowRelief[windowRelief.size() / 2];
        const u32 windows =
            glm::max(1u, static_cast<u32>(windowRelief.size()));
        u32 typeCounts[3] = { 0, 0, 0 };
        for (const u8 type : eventTypes) {
            ++typeCounts[type];
        }
        const u32 dominantType =
            glm::max(typeCounts[0], glm::max(typeCounts[1], typeCounts[2]));
        const u32 n = glm::max(samples, 1u);
        MESSAGE(std::string(label), ": h [", minH, ", ", maxH, "] mean ",
                meanH / n, " (sea ", seaLevel, ")  samples ", samples,
                " (", skipped, " skipped: rim band / off map)");
        MESSAGE("  windows(250m)=", windowRelief.size(), " land (",
                seaWindows, " sea)  flat(<8m relief) ",
                100.0f * static_cast<f32>(flatWindows) / windows,
                "%  median relief ", medianRelief, " m");
        MESSAGE("  families: socle ",
                100.0f * static_cast<f32>(socleWindows) / windows,
                "% (dont plateau ",
                100.0f * static_cast<f32>(plateauWindows) / windows,
                "%), versant ",
                100.0f * static_cast<f32>(versantWindows) / windows,
                "%, drame ",
                100.0f * static_cast<f32>(drameWindows) / windows,
                "%  (target 40/35/25, land only)");
        MESSAGE("  events: relief>25m ", reliefEvents, ", regime ",
                regimeEvents, ", water ", waterEvents,
                "  | mean event spacing ",
                gapCount ? gapSum / gapCount : 16000.0, " m, worst gap ",
                worstGap, " m  | dominant type ",
                eventTypes.empty()
                    ? 0.0f
                    : 100.0f * static_cast<f32>(dominantType) /
                          static_cast<f32>(eventTypes.size()),
                "%");
        MESSAGE("  slope: >15° ", 100.0f * static_cast<f32>(steep15) / n,
                "%, >30° ", 100.0f * static_cast<f32>(steep30) / n,
                "%  | max impassable stretch ", maxImpassable,
                " m  | water crossings/km ",
                static_cast<f32>(waterEvents) /
                    (static_cast<f32>(samples) * kStep / 1000.0f));
    };
    runTransect("E-W", 1.0f, 0.0f);
    runTransect("N-S", 0.0f, 1.0f);
    CHECK(true);
}

// Distant views from TRAVEL POINTS (a deterministic jittered grid of
// walkable spots over the map interior): per point, the horizon on 72
// azimuths to 18 km on the ERODED overview (the map's own truth, the
// far fallback beyond it), plus the two objective layers of the target
// (an alpine summit reachable at ~6 km, a marked hill at ~3 km).
// Acceptance: >= 30/72 open azimuths, a landmark > 2° beyond 3 km,
// both layers present from most points.
//   meadows-tests '-tc=vista diagnostic' -ns
TEST_CASE("vista diagnostic" * doctest::skip()) {
    const maptest::MapWorld& w = theMap();
    const f32 sea = w.seaLevel();
    const auto ha = [&](f32 x, f32 z) { return w.overviewHeight(x, z); };
    // Deterministic per-cell hash (splitmix-style; std::hash is not
    // portable across toolchains).
    const auto hash01 = [&](i32 cx, i32 cz, u32 salt) {
        u64 v = (static_cast<u64>(static_cast<u32>(cx)) << 32) ^
                static_cast<u32>(cz) ^ (static_cast<u64>(salt) << 17) ^
                kSeed;
        v ^= v >> 30;
        v *= 0xbf58476d1ce4e5b9ull;
        v ^= v >> 27;
        v *= 0x94d049bb133111ebull;
        v ^= v >> 31;
        return static_cast<f32>(v & 0xffffffu) / 16777215.0f;
    };

    // Travel points: jittered 6 km cells over the map interior, kept
    // when they land on walkable ground (dry, below the alpine band).
    struct Travel {
        f32 x, z, h;
    };
    vector<Travel> points;
    const f32 cell = 6000.0f;
    const f32 x0 = w.minX + kInterior;
    const f32 z0 = w.minZ + kInterior;
    const i32 cells = static_cast<i32>((w.mapSize - 2.0f * kInterior) / cell);
    u32 considered = 0;
    for (i32 cz = 0; cz < cells; ++cz) {
        for (i32 cx = 0; cx < cells; ++cx) {
            ++considered;
            const f32 x = x0 + (static_cast<f32>(cx) + 0.2f +
                                0.6f * hash01(cx, cz, 11)) *
                                   cell;
            const f32 z = z0 + (static_cast<f32>(cz) + 0.2f +
                                0.6f * hash01(cx, cz, 23)) *
                                   cell;
            const f32 h = ha(x, z);
            if (h > sea + 8.0f && h < sea + 450.0f) {
                points.push_back({ x, z, h });
            }
        }
    }
    MESSAGE("travel points kept: ", points.size(), "/", considered);

    u32 openOk = 0, landmarkOk = 0, summitOk = 0, hillOk = 0;
    for (const Travel& p : points) {
        const f32 eye = p.h + 1.7f;
        u32 openAzimuths = 0;
        bool landmark = false;
        for (u32 a = 0; a < 72; ++a) {
            const f32 azimuth = static_cast<f32>(a) * (6.2831853f / 72.0f);
            const f32 dx = std::cos(azimuth);
            const f32 dz = std::sin(azimuth);
            f32 bestAngle = -90.0f;
            f32 bestDist = 0.0f;
            for (f32 dist = 300.0f; dist <= 18000.0f; dist += 100.0f) {
                const f32 h = ha(p.x + dx * dist, p.z + dz * dist);
                const f32 angle = std::atan2(h - eye, dist) * 57.29578f;
                if (angle > bestAngle) {
                    bestAngle = angle;
                    bestDist = dist;
                }
            }
            if (bestDist > 2000.0f) {
                ++openAzimuths;
            }
            if (bestAngle > 2.0f && bestDist > 3000.0f) {
                landmark = true;
            }
        }
        // Objective layer 1: an alpine summit (>500 m over sea) within
        // 8 km. Layer 2: a marked hill (>120 m over its 1 km ring)
        // within 4 km.
        f32 dSummit = 1.0e9f;
        for (f32 sz = -8000.0f; sz <= 8000.0f; sz += 250.0f) {
            for (f32 sx = -8000.0f; sx <= 8000.0f; sx += 250.0f) {
                if (ha(p.x + sx, p.z + sz) > sea + 500.0f) {
                    dSummit = glm::min(dSummit, std::hypot(sx, sz));
                }
            }
        }
        f32 dHill = 1.0e9f;
        for (f32 sz = -4000.0f; sz <= 4000.0f; sz += 250.0f) {
            for (f32 sx = -4000.0f; sx <= 4000.0f; sx += 250.0f) {
                const f32 top = ha(p.x + sx, p.z + sz);
                if (top < sea + 60.0f) {
                    continue;
                }
                f32 ring = 0.0f;
                bool localMax = true;
                for (u32 k = 0; k < 8; ++k) {
                    const f32 angle =
                        static_cast<f32>(k) * (6.2831853f / 8.0f);
                    const f32 kx = std::cos(angle);
                    const f32 kz = std::sin(angle);
                    ring += ha(p.x + sx + kx * 1000.0f,
                               p.z + sz + kz * 1000.0f);
                    if (ha(p.x + sx + kx * 500.0f, p.z + sz + kz * 500.0f) >
                        top) {
                        localMax = false;
                        break;
                    }
                }
                if (localMax && top - ring / 8.0f > 120.0f) {
                    dHill = glm::min(dHill, std::hypot(sx, sz));
                }
            }
        }
        const bool open = openAzimuths >= 30;
        const bool summit = dSummit <= 8000.0f;
        const bool hill = dHill <= 4000.0f;
        openOk += open;
        landmarkOk += landmark;
        summitOk += summit;
        hillOk += hill;
        MESSAGE("point (", p.x, ", ", p.z, ") h=", p.h, ": open az ",
                openAzimuths, "/72",
                std::string(landmark ? "" : "  NO-LANDMARK"), "  summit ",
                dSummit < 1.0e9f ? dSummit / 1000.0f : -1.0f, " km  hill ",
                dHill < 1.0e9f ? dHill / 1000.0f : -1.0f, " km");
    }
    const u32 n = glm::max<u32>(1, static_cast<u32>(points.size()));
    MESSAGE("summary: open>=30az ", openOk, "/", n, "  landmark>2°@3km ",
            landmarkOk, "/", n, "  alpine summit<=8km ", summitOk, "/", n,
            "  marked hill<=4km ", hillOk, "/", n);
    CHECK(true);
}

// 2-D family census of the map interior: every 250 m window of the
// published slices classified socle/versant/drame with its relief —
// the fair instrument for the 40/35/25 budget (a straight transect
// over- or under-samples one family), and the proof that calm ground
// is calm.
//   meadows-tests '-tc=family census*' -ns
TEST_CASE("family census diagnostic" * doctest::skip()) {
    const maptest::MapWorld& w = theMap();
    const ProceduralControls controls = controlsOf(w);
    constexpr f32 kWindow = 250.0f;
    u32 socle = 0, versant = 0, drame = 0, wet = 0, plateau = 0, rim = 0;
    vector<f32> socleRelief, versantRelief, allRelief;
    for (const auto& regionPtr : w.tp.base->regions) {
        const render::TerrainRegion& r = *regionPtr;
        const u32 stride = static_cast<u32>(kWindow / r.texelSize);
        // The kept ring (overlapMargin) belongs to the neighbour too:
        // census the slice's own rect only.
        const u32 skip = static_cast<u32>(w.params.overlapMargin / r.texelSize);
        for (u32 wz = skip; wz + stride <= r.height - skip; wz += stride) {
            for (u32 wx = skip; wx + stride <= r.width - skip; wx += stride) {
                const f32 cx =
                    r.originX + (static_cast<f32>(wx) + stride * 0.5f) *
                                    r.texelSize;
                const f32 cz =
                    r.originZ + (static_cast<f32>(wz) + stride * 0.5f) *
                                    r.texelSize;
                if (!w.inside(cx, cz, kInterior)) {
                    ++rim;
                    continue;
                }
                f32 minH = 1.0e9f, maxH = -1.0e9f;
                vector<f32> slopes;
                for (u32 row = wz; row < wz + stride; row += 2) {
                    for (u32 col = wx; col < wx + stride; col += 2) {
                        const size_t i =
                            static_cast<size_t>(row) * r.width + col;
                        const f32 h = r.heights[i];
                        minH = glm::min(minH, h);
                        maxH = glm::max(maxH, h);
                        if (col + 2 < wx + stride) {
                            slopes.push_back(std::abs(r.heights[i + 2] - h) /
                                             (2.0f * r.texelSize));
                        }
                    }
                }
                if (maxH < w.seaLevel() + 0.5f) {
                    ++wet;
                    continue;
                }
                const f32 relief = maxH - minH;
                allRelief.push_back(relief);
                std::sort(slopes.begin(), slopes.end());
                const f32 medianSlope = slopes[slopes.size() / 2];
                if (medianSlope > 0.5774f) {
                    ++drame;
                } else if (medianSlope < 0.1763f && relief < 15.0f) {
                    ++socle;
                    socleRelief.push_back(relief);
                    if (controls.at(cx, cz).plateau > 80.0f) {
                        ++plateau;
                    }
                } else {
                    ++versant;
                    versantRelief.push_back(relief);
                }
            }
        }
    }
    const auto median = [](vector<f32>& v) {
        if (v.empty()) {
            return 0.0f;
        }
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    const f32 land = glm::max(1.0f, static_cast<f32>(socle + versant + drame));
    MESSAGE("map interior 250m windows: socle ", 100.0f * socle / land,
            "% (dont plateau ", 100.0f * plateau / land, "%), versant ",
            100.0f * versant / land, "%, drame ", 100.0f * drame / land,
            "%  (", wet, " wet, ", rim, " rim-band)  target 40/35/25, "
            "land only");
    MESSAGE("median relief: all ", median(allRelief), " m, socle ",
            median(socleRelief), " m, versant ", median(versantRelief),
            " m");
    CHECK(land > 0.0f);
}

// Calm-family coverage of the map: how much of its dry stage-1 ground
// the `calm` field claims (control level + valley-floor fusion) — the
// input the erosion damps with. Re-bakes the map stage-1 (the field is
// not persisted; palier D1). Watch it against the ~40% socle budget.
//   meadows-tests '-tc=calm coverage*' -ns
TEST_CASE("calm coverage diagnostic" * doctest::skip()) {
    const maptest::MapWorld& w = theMap();
    const ProceduralControls controls = controlsOf(w);
    // The map stage-1 as game::bakeMap runs it.
    TileBakeParams mp = w.params;
    mp.tileSize = w.mapSize;
    mp.apron = kMapApron;
    mp.mapGrid.seed = w.params.worldSeed;
    mp.mapGrid.mapSize = w.mapSize;
    mp.mapGrid.seaLevel = w.seaLevel();
    const TileStage1 s1 = bakeTileStage1(mp, w.mapX, w.mapZ);
    u64 dry = 0, calm06 = 0, calm03 = 0, fromControl = 0;
    for (u32 row = 0; row < s1.sim.n; row += 2) {
        for (u32 col = 0; col < s1.sim.n; col += 2) {
            const f32 x = s1.sim.x(col);
            const f32 z = s1.sim.z(row);
            if (!w.inside(x, z, kInterior)) {
                continue;
            }
            const size_t i = static_cast<size_t>(row) * s1.sim.n + col;
            if (s1.eroded[i] <= w.seaLevel()) {
                continue;
            }
            ++dry;
            calm06 += s1.calm[i] > 0.6f;
            calm03 += s1.calm[i] > 0.3f;
            fromControl += controls.at(x, z).calm > 0.6f;
        }
    }
    const f64 d = static_cast<f64>(glm::max<u64>(dry, 1));
    MESSAGE("map stage-1 (interior): dry cells ", dry, "  calm>0.6 ",
            100.0 * calm06 / d, "%  calm>0.3 ", 100.0 * calm03 / d,
            "%  control-only calm>0.6 ", 100.0 * fromControl / d, "%");
    CHECK(dry > 0);
}

TEST_CASE("snow coverage diagnostic" * doctest::skip()) {
    // Snow-line calibration instrument: measures the snow WEIGHT
    // coverage of the map interior under several (base snow line,
    // biome offsets) configs through the real materialWeightsAt path
    // (blended attributes + wander field included). "full" = weight >
    // 0.5, "touched" = the deposition overlay's band has begun (h >
    // line - 90). Altitude bands locate where the snow lives.
    const maptest::MapWorld& w = theMap();
    const f32 seaLevel = w.seaLevel();
    render::TerrainParams tp = w.tp;

    struct Config {
        const char* label;
        f32 snowLine;
        f32 arid;
        f32 alpine;
        f32 tundra;
    };
    // "adopte" is the shipped landscape.toml config, the others bracket
    // it one notch either way for future retuning.
    const Config configs[] = {
        { "adopte  ", 900.0f, 150.0f, -180.0f, -300.0f },
        { "var-950 ", 950.0f, 150.0f, -150.0f, -250.0f },
        { "var-850 ", 850.0f, 200.0f, -200.0f, -350.0f },
    };
    const auto biomeSetFor = [](const Config& c) {
        auto set = std::make_shared<render::BiomeSet>();
        set->table.resize(6);
        set->table[4].rockiness = 0.35f;
        set->table[4].grassPresence = 0.55f;
        set->table[4].snowLineOffset = -60.0f;
        set->table[4].temperature = -0.2f;
        set->table[4].wetness = 0.25f;
        set->table[5].sandiness = 0.35f;
        set->table[5].grassPresence = 0.6f;
        set->table[5].snowLineOffset = 80.0f;
        set->table[5].temperature = 0.35f;
        set->table[5].wetness = 0.15f;
        set->table[1].sandiness = 0.7f;
        set->table[1].grassPresence = 0.25f;
        set->table[1].snowLineOffset = c.arid;
        set->table[1].temperature = 0.6f;
        set->table[1].wetness = 0.1f;
        set->table[2].rockiness = 0.6f;
        set->table[2].grassPresence = 0.6f;
        set->table[2].snowLineOffset = c.alpine;
        set->table[2].temperature = -0.4f;
        set->table[3].rockiness = 0.3f;
        set->table[3].grassPresence = 0.4f;
        set->table[3].snowLineOffset = c.tundra;
        set->table[3].temperature = -0.8f;
        return set;
    };
    constexpr f32 kBandEdges[] = { 300.0f, 600.0f, 900.0f, 1200.0f };
    for (const Config& c : configs) {
        tp.snowLine = c.snowLine;
        tp.biomes = biomeSetFor(c);
        u32 land = 0;
        u32 full = 0;
        u32 touched = 0;
        u32 bandLand[5] = {};
        u32 bandFull[5] = {};
        for (f32 z = w.minZ + kInterior; z < w.maxZ - kInterior; z += 48.0f) {
            for (f32 x = w.minX + kInterior; x < w.maxX - kInterior;
                 x += 48.0f) {
                const f32 h = render::terrain::height(tp, x, z);
                if (h < seaLevel + 0.5f) {
                    continue;
                }
                ++land;
                const Vec3 n = render::terrain::normal(tp, x, z);
                const auto wts =
                    render::terrain::materialWeightsAt(tp, x, z, h, n);
                const auto fields = render::terrain::regionFieldsAt(tp, x, z);
                const f32 line = c.snowLine + fields.snowLineOffset;
                u32 band = 0;
                while (band < 4 && h >= kBandEdges[band]) {
                    ++band;
                }
                ++bandLand[band];
                if (wts.snow > 0.5f) {
                    ++full;
                    ++bandFull[band];
                }
                if (h > line - 90.0f) {
                    ++touched;
                }
            }
        }
        const auto pct = [](u32 num, u32 den) {
            return den ? 100.0f * static_cast<f32>(num) /
                             static_cast<f32>(den)
                       : 0.0f;
        };
        MESSAGE(std::string(c.label), " base ", c.snowLine, " offsets(",
                c.arid, "/", c.alpine, "/", c.tundra, "): full ",
                pct(full, land), "%  touched ", pct(touched, land), "%  (",
                land, " land samples)");
        MESSAGE("   full by band  <300m ", pct(bandFull[0], bandLand[0]),
                "%  300-600 ", pct(bandFull[1], bandLand[1]), "%  600-900 ",
                pct(bandFull[2], bandLand[2]), "%  900-1200 ",
                pct(bandFull[3], bandLand[3]), "%  >1200 ",
                pct(bandFull[4], bandLand[4]), "%");
    }
    CHECK(true);
}

TEST_CASE("biome locator diagnostic" * doctest::skip()) {
    // Where is each biome on the map? Scans the control fields over the
    // map rect (controls only — no bake) and prints, per palette id,
    // the nearest LAND occurrence to the spawn plus a far alternate,
    // with the overview height so the console/fly coordinate can be
    // pasted directly (x, y, z). The spawn is THE game's (shared probe).
    const maptest::MapWorld& w = theMap();
    const ProceduralControls controls = controlsOf(w);
    const Vec3 spawn = spawnOf(w);
    const f32 px = spawn.x;
    const f32 pz = spawn.z;
    MESSAGE("spawn (shared probe): (", static_cast<i32>(px), ", ",
            static_cast<i32>(spawn.y + 2.0f), ", ", static_cast<i32>(pz),
            ")");
    const char* names[] = { "temperate", "arid",      "alpine",
                            "tundra",    "subalpine", "steppe" };
    struct Hit {
        f32 x { 0.0f };
        f32 z { 0.0f };
        f32 d { 1.0e18f };
    };
    Hit nearest[6];
    Hit alternate[6]; // nearest beyond 6 km — a second spot to try
    u64 counts[6] = {};
    u64 landSamples = 0;
    for (f32 z = w.minZ + kInterior; z <= w.maxZ - kInterior; z += 96.0f) {
        for (f32 x = w.minX + kInterior; x <= w.maxX - kInterior;
             x += 96.0f) {
            const ControlSample s = controls.at(x, z);
            if (s.sea || s.biome >= 6) {
                continue;
            }
            ++landSamples;
            ++counts[s.biome];
            const f32 dx = x - px;
            const f32 dz = z - pz;
            const f32 d = dx * dx + dz * dz;
            if (d < nearest[s.biome].d) {
                nearest[s.biome] = { x, z, d };
            }
            if (d > 6000.0f * 6000.0f && d < alternate[s.biome].d) {
                alternate[s.biome] = { x, z, d };
            }
        }
    }
    for (u32 b = 0; b < 6; ++b) {
        MESSAGE(std::string(names[b]), ": ",
                landSamples ? 100.0 * static_cast<f64>(counts[b]) /
                                  static_cast<f64>(landSamples)
                            : 0.0,
                "% of the interior land");
    }
    // Patch geometry at the nearest steppe hit: how big is the zone the
    // player is sent to, and how strong does the blended sandiness (the
    // shader's aridity signal) actually get there?
    if (nearest[5].d < 1.0e18f) {
        const f32 cx = nearest[5].x;
        const f32 cz = nearest[5].z;
        u32 steppe = 0;
        u32 arid = 0;
        u32 total = 0;
        f32 maxSand = 0.0f;
        for (f32 z = cz - 750.0f; z <= cz + 750.0f; z += 24.0f) {
            for (f32 x = cx - 750.0f; x <= cx + 750.0f; x += 24.0f) {
                const ControlSample s = controls.at(x, z);
                ++total;
                steppe += s.biome == 5 ? 1 : 0;
                arid += s.biome == 1 ? 1 : 0;
                f32 sand = 0.0f;
                for (const Vec2 o : { Vec2 { 0, 0 }, Vec2 { 32, 0 },
                                      Vec2 { -32, 0 }, Vec2 { 0, 32 },
                                      Vec2 { 0, -32 } }) {
                    const u8 id = controls.at(x + o.x, z + o.y).biome;
                    const f32 wgt = (o.x == 0.0f && o.y == 0.0f) ? 2.0f
                                                                 : 1.0f;
                    sand += wgt * (id == 1 ? 0.7f : id == 5 ? 0.35f : 0.0f);
                }
                maxSand = glm::max(maxSand, sand / 6.0f);
            }
        }
        MESSAGE("steppe patch @nearest: steppe ",
                100.0f * static_cast<f32>(steppe) / static_cast<f32>(total),
                "%  arid ",
                100.0f * static_cast<f32>(arid) / static_cast<f32>(total),
                "% of the 1.5 km box, max blended sandiness ", maxSand);
    }
    for (u32 b = 0; b < 6; ++b) {
        const auto report = [&](const char* tag, const Hit& hit) {
            const std::string label = std::string(names[b]) + " " + tag;
            if (hit.d >= 1.0e18f) {
                MESSAGE(label, ": none on the map interior");
                return;
            }
            const f32 y = w.overviewHeight(hit.x, hit.z);
            MESSAGE(label, ": (", static_cast<i32>(hit.x), ", ",
                    static_cast<i32>(y + 40.0f), ", ",
                    static_cast<i32>(hit.z), ")  a ",
                    static_cast<i32>(std::sqrt(hit.d)), " m du spawn");
        };
        report("nearest", nearest[b]);
        report("alt>6km", alternate[b]);
    }
    CHECK(true);
}

TEST_CASE("lake census diagnostic" * doctest::skip()) {
    // Lake size distribution over the whole map: where does the puddle
    // tail end and the real lakes begin? Areas from the flooded masks
    // (never the bbox), max depth from level - min published ground.
    // Plus the river runs by tier and the fords.
    const maptest::MapWorld& w = theMap();
    struct Bucket {
        f32 maxArea; // m²
        const char* label;
        u32 count { 0 };
        f32 deepest { 0.0f };
    };
    Bucket buckets[] = {
        { 1000.0f, "<0.1ha  " },
        { 5000.0f, "0.1-0.5 " },
        { 20000.0f, "0.5-2ha " },
        { 100000.0f, "2-10ha  " },
        { 1.0e18f, ">10ha   " },
    };
    u32 total = 0;
    u32 dug = 0;
    u32 tierCount[3] = {};
    u32 fordCount = 0;
    f32 fleuveLength = 0.0f;
    for (const River& river : w.rivers) {
        ++tierCount[glm::min<u32>(river.tier, 2)];
        fordCount += static_cast<u32>(river.fords.size());
        if (river.tier == 2 && river.points.size() >= 2) {
            for (size_t s = 0; s + 1 < river.points.size(); ++s) {
                fleuveLength += std::hypot(
                    river.points[s + 1].x - river.points[s].x,
                    river.points[s + 1].z - river.points[s].z);
            }
            const RiverPoint& mid = river.points[river.points.size() / 2];
            MESSAGE("  fleuve run: mid (", static_cast<i32>(mid.x), ", ",
                    static_cast<i32>(mid.surface), ", ",
                    static_cast<i32>(mid.z), "), hw ", mid.halfWidth, ", ",
                    river.points.size(), " pts");
        }
    }
    for (const Lake& lake : w.lakes) {
        if (lake.dug) {
            ++dug; // placed ponds: design features, never filtered
            continue;
        }
        u32 wet = 0;
        f32 depth = 0.0f;
        for (u32 mz = 0; mz < lake.maskHeight; ++mz) {
            for (u32 mx = 0; mx < lake.maskWidth; ++mx) {
                if (!lake.mask[static_cast<size_t>(mz) * lake.maskWidth +
                               mx]) {
                    continue;
                }
                ++wet;
                const f32 x =
                    lake.minX + (static_cast<f32>(mx) + 0.5f) * lake.maskTexel;
                const f32 z =
                    lake.minZ + (static_cast<f32>(mz) + 0.5f) * lake.maskTexel;
                depth = glm::max(depth, lake.level - w.height(x, z));
            }
        }
        const f32 area =
            static_cast<f32>(wet) * lake.maskTexel * lake.maskTexel;
        ++total;
        for (Bucket& b : buckets) {
            if (area <= b.maxArea) {
                ++b.count;
                b.deepest = glm::max(b.deepest, depth);
                break;
            }
        }
    }
    const f32 landKm2 = w.mapSize * w.mapSize / 1.0e6f;
    MESSAGE("natural lakes on the map: ", total, " (", total / landKm2 * 16.0f,
            " per 4x4 km)  (+ ", dug, " placed ponds, never filtered)");
    MESSAGE("river runs by tier: ruisseau ", tierCount[0], "  riviere ",
            tierCount[1], "  fleuve ", tierCount[2], " (", fleuveLength / 1000.0f,
            " km)  | fords ", fordCount);
    for (const Bucket& b : buckets) {
        MESSAGE("  ", std::string(b.label), ": ", b.count, "  (deepest ",
                b.deepest, " m)");
    }
    CHECK(true);
}

TEST_CASE("river wetness diagnostic" * doctest::skip()) {
    // The ground truth of "l'eau est continue dans les creusements":
    // walks every published river run's centerline at 2 m and probes
    // the published terrain against the ribbon surface — DRY means the
    // water sheet is clipped under the ground there. Also lists the run
    // ends (each end is a place the ribbon dissolves — too many of them
    // and the course reads as broken puddles).
    const maptest::MapWorld& w = theMap();
    u32 samples = 0;
    u32 dry = 0;
    f32 worstDryRun = 0.0f;
    f32 totalLen = 0.0f;
    u32 runs = 0;
    u32 shortRuns = 0; // < 100 m: crop confetti, all ends dissolving
    f32 worstFlat = 0.0f; // longest LEVEL surface stretch, tier 2 —
                          // the "fleuve reads as a lake" measure
    for (const River& river : w.rivers) {
        if (river.points.size() < 2) {
            continue;
        }
        ++runs;
        if (river.tier == 2) {
            f32 flat = 0.0f;
            for (size_t s = 0; s + 1 < river.points.size(); ++s) {
                const f32 len =
                    std::hypot(river.points[s + 1].x - river.points[s].x,
                               river.points[s + 1].z - river.points[s].z);
                if (river.points[s].surface - river.points[s + 1].surface <
                    0.01f) {
                    flat += len;
                    worstFlat = glm::max(worstFlat, flat);
                } else {
                    flat = 0.0f;
                }
            }
        }
        f32 runLen = 0.0f;
        f32 dryStretch = 0.0f;
        for (size_t s = 0; s + 1 < river.points.size(); ++s) {
            const RiverPoint& a = river.points[s];
            const RiverPoint& b = river.points[s + 1];
            const f32 len = std::hypot(b.x - a.x, b.z - a.z);
            runLen += len;
            const i32 n = glm::max(static_cast<i32>(len / 2.0f), 1);
            for (i32 i = 0; i < n; ++i) {
                const f32 t = static_cast<f32>(i) / static_cast<f32>(n);
                const f32 x = glm::mix(a.x, b.x, t);
                const f32 z = glm::mix(a.z, b.z, t);
                const f32 surface = glm::mix(a.surface, b.surface, t);
                const f32 h = w.height(x, z);
                ++samples;
                if (h > surface - 0.05f) {
                    ++dry;
                    dryStretch += 2.0f;
                    if (dryStretch > worstDryRun) {
                        worstDryRun = dryStretch;
                        MESSAGE("    dry at (", static_cast<i32>(x), ", ",
                                static_cast<i32>(z), "): terrain ", h,
                                " vs surface ", surface, " (hw ",
                                glm::mix(a.halfWidth, b.halfWidth, t),
                                ", stretch ", dryStretch, " m)");
                    }
                } else {
                    dryStretch = 0.0f;
                }
            }
        }
        totalLen += runLen;
        shortRuns += runLen < 100.0f ? 1 : 0;
    }
    MESSAGE("  ", runs, " runs, ", totalLen / 1000.0f, " km total, ",
            shortRuns, " runs < 100 m");
    MESSAGE("  centerline dry: ",
            100.0f * static_cast<f32>(dry) /
                static_cast<f32>(glm::max(samples, 1u)),
            "%  worst continuous dry stretch ", worstDryRun, " m");
    MESSAGE("  longest LEVEL fleuve surface: ", worstFlat, " m");
    CHECK(true);
}

TEST_CASE("fleuve locator diagnostic" * doctest::skip()) {
    // Where are the fleuves? Master-network courses of the map's
    // super-region (no bake needed — the promotion follows these very
    // polylines). Prints, per course, its nearest point to the spawn and
    // its biggest-area node, as pasteable (x, y, z) on the overview.
    const maptest::MapWorld& w = theMap();
    const ProceduralControls controls = controlsOf(w);
    MasterNetworkParams network = w.params.network;
    network.seaLevel = w.seaLevel();
    const Vec3 spawn = spawnOf(w);
    const f32 px = spawn.x;
    const f32 pz = spawn.z;
    const auto rivers = masterRiversNear(controls, w.params.macro, network,
                                         w.minX, w.minZ, w.maxX, w.maxZ);
    MESSAGE("master courses touching the map: ", rivers.size());
    struct Entry {
        f32 dist;
        f32 nx, nz; // nearest node to spawn
        f32 bx, bz; // biggest-area node (the wide stretch)
        f32 area;
        f32 length;
    };
    vector<Entry> entries;
    for (const auto& river : rivers) {
        if (river.nodes.size() < 2) {
            continue;
        }
        Entry e { 1.0e30f, 0, 0, 0, 0, 0.0f, 0.0f };
        for (size_t i = 0; i < river.nodes.size(); ++i) {
            const MasterNode& node = river.nodes[i];
            const f32 dx = node.x - px;
            const f32 dz = node.z - pz;
            const f32 d = std::sqrt(dx * dx + dz * dz);
            if (d < e.dist) {
                e.dist = d;
                e.nx = node.x;
                e.nz = node.z;
            }
            if (node.area > e.area) {
                e.area = node.area;
                e.bx = node.x;
                e.bz = node.z;
            }
            if (i > 0) {
                e.length += std::hypot(node.x - river.nodes[i - 1].x,
                                       node.z - river.nodes[i - 1].z);
            }
        }
        entries.push_back(e);
    }
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.dist < b.dist; });
    for (size_t i = 0; i < glm::min<size_t>(entries.size(), 8); ++i) {
        const Entry& e = entries[i];
        const f32 yn = w.overviewHeight(e.nx, e.nz);
        const f32 yb = w.overviewHeight(e.bx, e.bz);
        MESSAGE("fleuve ", i, ": ", static_cast<i32>(e.length / 1000),
                " km, nearest (", static_cast<i32>(e.nx), ", ",
                static_cast<i32>(yn + 30.0f), ", ", static_cast<i32>(e.nz),
                ") a ", static_cast<i32>(e.dist), " m du spawn | large a (",
                static_cast<i32>(e.bx), ", ", static_cast<i32>(yb + 30.0f),
                ", ", static_cast<i32>(e.bz), ")");
    }
    CHECK(true);
}

TEST_CASE("fleuve continuity diagnostic" * doctest::skip()) {
    // Seam continuity of the published water: a run cut by an interior
    // slice line must continue in the neighbour slice (one hydrology,
    // clipped per slice). Lists every run end on a slice line without a
    // matching end across it.
    const maptest::MapWorld& w = theMap();
    const f32 t = w.params.tileSize;
    const auto onSliceLine = [&](f32 x, f32 z) {
        const f32 fx = x - std::round(x / t) * t;
        const f32 fz = z - std::round(z / t) * t;
        const bool interiorX =
            x > w.minX + t * 0.5f && x < w.maxX - t * 0.5f;
        const bool interiorZ =
            z > w.minZ + t * 0.5f && z < w.maxZ - t * 0.5f;
        return (std::abs(fx) < 6.0f && interiorX) ||
               (std::abs(fz) < 6.0f && interiorZ);
    };
    struct End {
        f32 x, z;
        u8 tier;
        f32 hw;
        size_t run;
    };
    vector<End> ends;
    for (size_t r = 0; r < w.rivers.size(); ++r) {
        const River& river = w.rivers[r];
        if (river.points.size() < 2) {
            continue;
        }
        for (const RiverPoint* p : { &river.points.front(),
                                     &river.points.back() }) {
            if (onSliceLine(p->x, p->z)) {
                ends.push_back({ p->x, p->z, river.tier, p->halfWidth, r });
            }
        }
    }
    u32 matched = 0, unmatched = 0, tierMismatch = 0;
    for (size_t i = 0; i < ends.size(); ++i) {
        const End& a = ends[i];
        bool found = false;
        for (size_t j = 0; j < ends.size(); ++j) {
            if (i == j || ends[j].run == a.run) {
                continue;
            }
            const End& b = ends[j];
            if (std::hypot(a.x - b.x, a.z - b.z) < 12.0f) {
                found = true;
                if (a.tier != b.tier) {
                    ++tierMismatch;
                }
                break;
            }
        }
        if (found) {
            ++matched;
        } else {
            ++unmatched;
            if (unmatched <= 12) {
                MESSAGE("  run end on a slice line without a continuation: (",
                        static_cast<i32>(a.x), ", ", static_cast<i32>(a.z),
                        ") tier ", static_cast<u32>(a.tier), " hw ", a.hw);
            }
        }
    }
    MESSAGE("run ends on interior slice lines: ", ends.size(), "  matched ",
            matched, "  unmatched ", unmatched, "  tier mismatch ",
            tierMismatch);
    CHECK(true);
}

// UV health of the scanned rocks (docs/PAYSAGE.md §1.7 props): per model,
// the per-triangle texel-stretch distribution (world area vs uv area)
// BEFORE and AFTER decimation — the striped-face hunt (the texture
// not wrapping around the pale boulder).
//   meadows-tests '-tc=rock uv diagnostic' -ns
TEST_CASE("rock uv diagnostic" * doctest::skip()) {
    const char* kRocks[] = {
        "game/data/base/models/scans/stone_01/stone_01.gltf",
        "game/data/base/models/scans/rock_moss_set_01/rock_moss_set_01.gltf",
        "game/data/base/models/scans/rock_boulder_dry/rock_boulder_dry.gltf",
        "game/data/base/models/scans/boulder_01/boulder_01.gltf",
    };
    const auto stats = [](const render::MeshData& mesh, const char* tag) {
        u32 degenerate = 0;
        u32 stretched = 0;
        u32 tris = 0;
        f32 uvMinX = 1.0e9f, uvMaxX = -1.0e9f;
        f32 uvMinY = 1.0e9f, uvMaxY = -1.0e9f;
        for (const render::MeshVertex& v : mesh.vertices) {
            uvMinX = glm::min(uvMinX, v.uv.x);
            uvMaxX = glm::max(uvMaxX, v.uv.x);
            uvMinY = glm::min(uvMinY, v.uv.y);
            uvMaxY = glm::max(uvMaxY, v.uv.y);
        }
        for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            const auto& a = mesh.vertices[mesh.indices[i]];
            const auto& b = mesh.vertices[mesh.indices[i + 1]];
            const auto& c = mesh.vertices[mesh.indices[i + 2]];
            const f32 wArea = 0.5f * glm::length(glm::cross(
                b.position - a.position, c.position - a.position));
            const Vec2 e1 = b.uv - a.uv;
            const Vec2 e2 = c.uv - a.uv;
            const f32 uvArea =
                0.5f * std::abs(e1.x * e2.y - e1.y * e2.x);
            if (wArea < 1.0e-8f) {
                continue;
            }
            ++tris;
            const f32 texelDensity = uvArea / wArea;
            if (texelDensity < 1.0e-5f) {
                ++degenerate; // stretched to streaks
            } else if (texelDensity < 1.0e-3f) {
                ++stretched;
            }
        }
        MESSAGE("  ", std::string(tag), ": ", tris, " tris, uv x[", uvMinX, ",",
                uvMaxX, "] y[", uvMinY, ",", uvMaxY, "], degenerate ",
                degenerate, ", stretched ", stretched);
    };
    for (const char* path : kRocks) {
        auto mesh = assets::loadGltfMesh(path);
        if (!mesh) {
            MESSAGE(std::string(path), ": LOAD FAILED");
            continue;
        }
        MESSAGE(std::string(path), ":");
        stats(*mesh, "source");
        render::MeshData simplified = *mesh;
        assets::simplifyMesh(simplified, 700);
        stats(simplified, "decimated 700");
    }
    CHECK(true);
}
