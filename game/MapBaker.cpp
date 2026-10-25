#include "game/MapBaker.hpp"

#include "engine/terrain/generation/GridOps.hpp"

#include "game/TerrainGenTuning.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <thread>

#include "engine/core/ConcurrentQueue.hpp"
#include "engine/core/Hash.hpp"
#include "engine/core/Jobs.hpp"
#include "engine/core/Log.hpp"
#include "game/TerrainBakeStreamer.hpp"
#include "world/terrain/TerrainRegions.hpp"

namespace game {

namespace {

using namespace render::terraingen;

f64 secondsSince(const std::chrono::steady_clock::time_point& start) {
    return std::chrono::duration<f64> { std::chrono::steady_clock::now() -
                                        start }
        .count();
}

} // namespace

namespace {
constexpr char kOverviewMagic[4] = { 'M', 'O', 'V', '1' };
constexpr u32 kOverviewStep = 4; // 16 m stage-1 texels -> 64 m
} // namespace

std::optional<MapOverview> loadMapOverview(
    const std::filesystem::path& mapDir) {
    std::ifstream file { mapDir / "overview.bin", std::ios::binary };
    if (!file) {
        return std::nullopt;
    }
    char magic[4] = {};
    MapOverview out;
    const auto read = [&](auto& value) {
        file.read(reinterpret_cast<char*>(&value), sizeof(value));
    };
    file.read(magic, 4);
    read(out.grid.originX);
    read(out.grid.originZ);
    read(out.grid.texelSize);
    read(out.grid.n);
    if (!file || std::memcmp(magic, kOverviewMagic, 4) != 0 ||
        out.grid.n < 2 || out.grid.n > 8192) {
        LOG_WARN("Map cache: rejected {} (corrupt overview)",
                 (mapDir / "overview.bin").string());
        return std::nullopt;
    }
    out.heights.resize(out.grid.cells());
    file.read(reinterpret_cast<char*>(out.heights.data()),
              static_cast<std::streamsize>(out.heights.size() *
                                           sizeof(f32)));
    if (!file) {
        return std::nullopt;
    }
    return out;
}

u64 mapBakeKey(const TileBakeParams& params, i32 tilesPerSide) {
    u64 h = core::kFnv64Basis;
    const auto mix = [&](const void* data, size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < size; ++i) {
            h ^= bytes[i];
            h *= core::kFnv64Prime;
        }
    };
    const auto mixF = [&](f32 v) { mix(&v, sizeof(v)); };
    const auto mixU = [&](u32 v) { mix(&v, sizeof(v)); };
    mixU(kTileBakeVersion);
    mixU(static_cast<u32>(tilesPerSide));
    mixU(params.worldSeed);
    mixU(params.controls.seed);
    mixF(params.tileSize);
    mixF(params.macro.seaLevel);
    mixF(params.macro.recurveLow);
    mixF(params.macro.recurveMid);
    mixF(params.macro.recurveHigh);
    mixU(params.mapGrid.valid ? 1u : 0u);
    // The generation pupitre (every TerrainGenTuningForm field): an
    // edit is a different map, re-baked on demand.
    const u64 gen = hashTerrainGenTuning(params);
    mix(&gen, sizeof(gen));
    const u64 zones = render::terraingen::hashParams(params.controls.zones);
    mix(&zones, sizeof(zones));
    return h;
}

bool mapBakedAndValid(const std::filesystem::path& cacheDir, i32 mapX,
                      i32 mapZ, i32 tilesPerSide,
                      const TileBakeParams* params) {
    std::ifstream manifest {
        mapCacheDir(cacheDir, mapX, mapZ) / "manifest.txt"
    };
    if (!manifest) {
        return false;
    }
    str key;
    u32 headerVersion = 0;
    if (!(manifest >> key >> headerVersion) || key != "meadows-map" ||
        headerVersion != kMapBakeVersion) {
        return false;
    }
    i32 mx = 0;
    i32 mz = 0;
    f32 mapSize = 0.0f;
    f32 tileSize = 0.0f;
    i32 tps = 0;
    u32 seed = 0;
    u32 bakeVersion = 0;
    str k2, k3, k4, k5, k6, k7;
    if (!(manifest >> k2 >> mx >> mz >> k3 >> mapSize >> k4 >>
          tileSize >> k5 >> tps >> k6 >> seed >> k7 >> bakeVersion)) {
        return false;
    }
    str k8;
    u64 storedKey = 0;
    if (!(manifest >> k8 >> storedKey) || k8 != "key") {
        return false;
    }
    return mx == mapX && mz == mapZ && tps == tilesPerSide &&
           bakeVersion == render::terraingen::kTileBakeVersion &&
           (!params || storedKey == mapBakeKey(*params, tilesPerSide));
}

std::filesystem::path mapCacheDir(const std::filesystem::path& cacheDir,
                                  i32 mapX, i32 mapZ) {
    char dir[48];
    std::snprintf(dir, sizeof(dir), "map_%d_%d", mapX, mapZ);
    return cacheDir / dir;
}


namespace {

constexpr f32 kOvUnset = -1.0e30f;

// One bake of one map: the inputs, the state the stages hand down (the
// global stage-1, the slice grounds, the map hydrology, the overview
// lattice) and the stats returned. Stages run in order; each returns
// false to stop the bake (cancelled or failed) with `stats` carrying
// what was done.
struct MapBakeRun {
    MapBakeRun(const TileBakeParams& bakeParams, i32 mx, i32 mz,
               const std::filesystem::path& cache, core::JobSystem* js,
               i32 tps, const std::function<void(u32, u32)>& prog)
        : params { bakeParams }, mapX { mx }, mapZ { mz },
          cacheDir { cache }, jobs { js }, tilesPerSide { tps },
          progress { prog }, cancel { js ? &js->stopFlag() : nullptr },
          tx0 { mx * tps }, tz0 { mz * tps },
          total { static_cast<u32>(tps) * static_cast<u32>(tps) } {}

    bool cancelled() const {
        return cancel && cancel->load(std::memory_order_relaxed);
    }

    const TileBakeParams& params;
    i32 mapX;
    i32 mapZ;
    const std::filesystem::path& cacheDir;
    core::JobSystem* jobs;
    i32 tilesPerSide;
    const std::function<void(u32, u32)>& progress;
    const std::atomic<bool>* cancel;
    i32 tx0;
    i32 tz0;
    u32 total;

    std::filesystem::path mapDir;
    TileBakeParams mapParams;
    TileStage1 mapS1;
    std::chrono::steady_clock::time_point s2Start;
    vector<FinalizeResult> grounds;
    MapHydrology mapHydro;
    render::terraingen::GridSpec ovGrid;
    vector<f32> ovFine;
    MapBakeStats stats;
};

bool createMapDir(MapBakeRun& run) {
    run.mapDir = mapCacheDir(run.cacheDir, run.mapX, run.mapZ);
    std::error_code ec;
    std::filesystem::create_directories(run.mapDir, ec);
    if (ec) {
        LOG_ERROR("map bake: cannot create {} ({})", run.mapDir.string(),
                  ec.message());
        return false;
    }
    return true;
}

// The global stage-1: same recipe, map-sized window, the production
// apron (rim basins resolve inside it; its rim is the base level).
bool bakeGlobalStage1(MapBakeRun& run) {
    const TileBakeParams& params = run.params;
    TileBakeParams& mapParams = run.mapParams;
    mapParams = params;
    mapParams.tileSize =
        params.tileSize * static_cast<f32>(run.tilesPerSide);
    mapParams.apron = kMapApron;
    // The border-transition grid: pure function of the world seed and
    // the map lattice — nothing per-map to fill.
    if (mapParams.mapGrid.valid) {
        mapParams.mapGrid.seed = params.worldSeed;
        mapParams.mapGrid.mapSize = mapParams.tileSize;
        mapParams.mapGrid.seaLevel = params.macro.seaLevel;
    }
    const auto s1Start = std::chrono::steady_clock::now();
    run.mapS1 = bakeTileStage1(mapParams, run.mapX, run.mapZ,
                               run.cancel);
    run.stats.stage1Seconds = secondsSince(s1Start);
    if (run.cancelled()) {
        run.stats.cancelled = true;
        return false; // partial stage-1: no slice, no manifest
    }
    LOG_INFO("map bake ({}, {}): global stage-1 {}x{} in {:.1f} s",
             run.mapX, run.mapZ, run.mapS1.sim.n, run.mapS1.sim.n,
             run.stats.stage1Seconds);
    return true;
}

// TWO-PHASE slice bake (docs/PAYSAGE.md §7.8):
// A. every slice's final GROUND (parallel, held in memory);
// B. ONE map hydrology ON those grounds — water placed on the
//    terrain the player walks is correct by construction;
// C. every slice's water carves + files (parallel).
// Phase A. The slice clock starts here and runs through phase C.
bool bakeSliceGrounds(MapBakeRun& run) {
    const TileBakeParams& params = run.params;
    const TileStage1& mapS1 = run.mapS1;
    const i32 tilesPerSide = run.tilesPerSide;
    const u32 total = run.total;
    run.s2Start = std::chrono::steady_clock::now();
    const MapDrainage drainage =
        computeMapDrainage(params, mapS1, run.mapX, run.mapZ, tilesPerSide);
    vector<FinalizeResult>& grounds = run.grounds;
    grounds = vector<FinalizeResult>(total);
    {
        core::ConcurrentQueue<i32> doneA;
        u32 landedA = 0;
        const auto bakeGround = [&](u32 slot, i32 tx, i32 tz) {
            if (!run.cancelled()) {
                grounds[slot] =
                    bakeSliceGround(params, tx, tz, mapS1, drainage,
                                    run.cancel);
            }
            doneA.push(1);
        };
        for (i32 dz = 0; dz < tilesPerSide; ++dz) {
            for (i32 dx = 0; dx < tilesPerSide; ++dx) {
                const u32 slot = static_cast<u32>(
                    dz * tilesPerSide + dx);
                const i32 tx = run.tx0 + dx;
                const i32 tz = run.tz0 + dz;
                if (run.jobs) {
                    run.jobs->enqueue(
                        [&, slot, tx, tz] { bakeGround(slot, tx, tz); });
                } else {
                    bakeGround(slot, tx, tz);
                }
            }
        }
        while (landedA < total) {
            i32 ok = 0;
            if (doneA.tryPop(ok)) {
                ++landedA;
                if (run.progress) {
                    run.progress(landedA, total * 2);
                }
            } else {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(20));
            }
        }
    }
    if (run.cancelled()) {
        run.stats.cancelled = true;
        return false;
    }
    return true;
}

// Border slices of CACHED neighbour maps: the hydrology window reads
// their published final ground past our rect, so the shared margin's
// water resolves on the terrain both maps show. A map baked alone (no
// neighbour cache) stays bit-exact.
vector<render::TerrainRegion> loadNeighbourBorderSlices(
    const MapBakeRun& run) {
    const i32 tilesPerSide = run.tilesPerSide;
    vector<render::TerrainRegion> neighbourRegions;
    const i32 offs[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 },
                             { 0, -1 } };
    for (const auto& off : offs) {
        const i32 nx = run.mapX + off[0];
        const i32 nz = run.mapZ + off[1];
        if (!mapBakedAndValid(run.cacheDir, nx, nz, tilesPerSide,
                              &run.params)) {
            continue;
        }
        const auto nDir = mapCacheDir(run.cacheDir, nx, nz);
        for (i32 k = 0; k < tilesPerSide; ++k) {
            // The neighbour's slice ROW/COLUMN along our shared
            // line (the waterMargin band lies inside it).
            const i32 tx =
                off[0] != 0
                    ? (off[0] > 0 ? nx * tilesPerSide
                                  : nx * tilesPerSide +
                                        tilesPerSide - 1)
                    : nx * tilesPerSide + k;
            const i32 tz =
                off[1] != 0
                    ? (off[1] > 0 ? nz * tilesPerSide
                                  : nz * tilesPerSide +
                                        tilesPerSide - 1)
                    : nz * tilesPerSide + k;
            char stem[64];
            std::snprintf(stem, sizeof(stem), "tile_%d_%d_v%u",
                          tx, tz, kTileBakeVersion);
            if (auto region = world::readTrgFile(
                    nDir / (str { stem } + ".trg"))) {
                neighbourRegions.push_back(std::move(*region));
            }
        }
    }
    if (!neighbourRegions.empty()) {
        LOG_INFO("map bake ({}, {}): {} neighbour border slice(s) "
                 "feed the hydrology margin",
                 run.mapX, run.mapZ, neighbourRegions.size());
    }
    return neighbourRegions;
}

// Phase B: the one map hydrology on the phase-A grounds.
bool extractHydrology(MapBakeRun& run) {
    const auto hydroStart = std::chrono::steady_clock::now();
    vector<const FinalizeResult*> groundPtrs;
    groundPtrs.reserve(run.grounds.size());
    for (const FinalizeResult& g : run.grounds) {
        groundPtrs.push_back(&g);
    }
    const vector<render::TerrainRegion> neighbourRegions =
        loadNeighbourBorderSlices(run);
    run.mapHydro = extractMapHydrology(
        run.params, run.mapS1, run.mapX, run.mapZ, run.tilesPerSide,
        groundPtrs, neighbourRegions, run.cancel);
    if (run.cancelled()) {
        run.stats.cancelled = true;
        return false;
    }
    const MapHydrology& mapHydro = run.mapHydro;
    LOG_INFO("map bake ({}, {}): map hydrology {}x{} in {:.1f} s — {} "
             "lake(s), {} river(s)",
             run.mapX, run.mapZ, mapHydro.window.n, mapHydro.window.n,
             secondsSince(hydroStart), mapHydro.hydro.lakes.size(),
             mapHydro.hydro.rivers.size());
    return true;
}

// Phase C: every slice's water carves + files, splatting the overview
// lattice as it lands. Closes the slice clock and the written count.
bool carveSlicesAndWrite(MapBakeRun& run) {
    const TileBakeParams& params = run.params;
    const TileStage1& mapS1 = run.mapS1;
    const MapHydrology& mapHydro = run.mapHydro;
    const i32 tilesPerSide = run.tilesPerSide;
    const u32 total = run.total;
    const auto& mapDir = run.mapDir;
    core::ConcurrentQueue<i32> done; // 1 = written, 0 = failed/cancelled
    u32 landed = 0;
    u32 written = 0;
    // Overview lattice, filled by the slices as they land: the far
    // ground must be the FINAL carved fine ground, not the pre-carve
    // window — the published water now seats on the fine floor, and a
    // far terrain without the carved canyons buried every ribbon
    // metres under its own décor (measured). Tile rects partition the
    // map, so each worker writes disjoint texels (no lock); the
    // completion queue orders the main thread's read. Texels no slice
    // owns (apron, window ring) keep the window/stage-1 fallback.
    render::terraingen::GridSpec& ovGrid = run.ovGrid;
    ovGrid.originX = mapS1.sim.originX;
    ovGrid.originZ = mapS1.sim.originZ;
    ovGrid.texelSize =
        mapS1.sim.texelSize * static_cast<f32>(kOverviewStep);
    ovGrid.n = (mapS1.sim.n - 1) / kOverviewStep + 1;
    vector<f32>& ovFine = run.ovFine;
    ovFine = vector<f32>(ovGrid.cells(), kOvUnset);
    const auto bakeSlice = [&](u32 slot, i32 tx, i32 tz) {
        if (run.cancelled()) {
            done.push(0);
            return;
        }
        TileBakeResult slice =
            bakeSliceWater(params, tx, tz, mapS1, mapHydro,
                           std::move(run.grounds[slot]), run.cancel);
        if (run.cancelled()) {
            done.push(0);
            return;
        }
        {
            const f32 tileMinX =
                static_cast<f32>(tx) * params.tileSize;
            const f32 tileMinZ =
                static_cast<f32>(tz) * params.tileSize;
            const auto& region = slice.region;
            for (u32 row = 0; row < ovGrid.n; ++row) {
                const f32 z = ovGrid.z(row);
                if (z < tileMinZ || z >= tileMinZ + params.tileSize) {
                    continue;
                }
                for (u32 col = 0; col < ovGrid.n; ++col) {
                    const f32 x = ovGrid.x(col);
                    if (x < tileMinX ||
                        x >= tileMinX + params.tileSize) {
                        continue;
                    }
                    const i32 ix = glm::clamp(
                        static_cast<i32>(std::lround(
                            (x - region.originX) /
                            region.texelSize)),
                        0, static_cast<i32>(region.width) - 1);
                    const i32 iz = glm::clamp(
                        static_cast<i32>(std::lround(
                            (z - region.originZ) /
                            region.texelSize)),
                        0, static_cast<i32>(region.height) - 1);
                    ovFine[static_cast<size_t>(row) * ovGrid.n +
                           col] =
                        region.heights[static_cast<size_t>(iz) *
                                           region.width +
                                       static_cast<size_t>(ix)];
                }
            }
        }
        char stem[64];
        std::snprintf(stem, sizeof(stem), "tile_%d_%d_v%u", tx, tz,
                      kTileBakeVersion);
        const bool ok =
            world::writeTrgFile(mapDir / (str { stem } + ".trg"),
                                slice.region) &&
            writeWaterFile(mapDir / (str { stem } + ".twb"),
                           slice.lakes, slice.rivers);
        if (!ok) {
            LOG_ERROR("map bake: cannot write slice ({}, {})", tx, tz);
        }
        done.push(ok ? 1 : 0);
    };
    for (i32 dz = 0; dz < tilesPerSide; ++dz) {
        for (i32 dx = 0; dx < tilesPerSide; ++dx) {
            const u32 slot = static_cast<u32>(dz * tilesPerSide + dx);
            const i32 tx = run.tx0 + dx;
            const i32 tz = run.tz0 + dz;
            if (run.jobs) {
                run.jobs->enqueue(
                    [&, slot, tx, tz] { bakeSlice(slot, tx, tz); });
            } else {
                bakeSlice(slot, tx, tz);
            }
        }
    }
    while (landed < total) {
        i32 ok = 0;
        if (done.tryPop(ok)) {
            ++landed;
            written += ok > 0 ? 1u : 0u;
            if (run.progress) {
                run.progress(total + landed, total * 2);
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    run.stats.sliceSeconds = secondsSince(run.s2Start);
    run.stats.slicesWritten = written;
    if (run.cancelled() || written != total) {
        run.stats.cancelled = run.cancelled();
        LOG_WARN("map bake ({}, {}): incomplete ({}/{} slices) — no "
                 "manifest written",
                 run.mapX, run.mapZ, written, total);
        return false;
    }
    return true;
}

// The overview: the shared surface decimated to 64 m (exact texel
// picks — rim and shaped apron included), the runtime fallback inside
// the map.
bool writeOverview(MapBakeRun& run) {
    const TileStage1& mapS1 = run.mapS1;
    const MapHydrology& mapHydro = run.mapHydro;
    const vector<f32>& ovFine = run.ovFine;
    MapOverview overview;
    overview.grid = run.ovGrid;
    overview.heights.resize(overview.grid.cells());
    for (u32 row = 0; row < overview.grid.n; ++row) {
        for (u32 col = 0; col < overview.grid.n; ++col) {
            const size_t idx =
                static_cast<size_t>(row) * overview.grid.n + col;
            // A slice-owned texel carries the FINAL carved fine
            // ground (splatted in phase C); outside the tiles, the
            // pierced window inside the hydrology window and the
            // stage-1 on the apron.
            if (ovFine[idx] > kOvUnset * 0.5f) {
                overview.heights[idx] = ovFine[idx];
                continue;
            }
            const f32 x = overview.grid.x(col);
            const f32 z = overview.grid.z(row);
            const f32 s1 =
                mapS1.eroded[static_cast<size_t>(row) *
                                 kOverviewStep * mapS1.sim.n +
                             static_cast<size_t>(col) *
                                 kOverviewStep];
            const render::terraingen::GridSpec& win =
                mapHydro.window;
            const f32 winMaxX =
                win.originX +
                static_cast<f32>(win.n - 1) * win.texelSize;
            const f32 winMaxZ =
                win.originZ +
                static_cast<f32>(win.n - 1) * win.texelSize;
            overview.heights[idx] =
                (x >= win.originX && x <= winMaxX &&
                 z >= win.originZ && z <= winMaxZ)
                    ? render::terraingen::bilinearWorld(
                          win, mapHydro.ground, x, z)
                    : s1;
        }
    }
    std::ofstream file { run.mapDir / "overview.bin",
                         std::ios::binary | std::ios::trunc };
    const auto write = [&](const auto& value) {
        file.write(reinterpret_cast<const char*>(&value),
                   sizeof(value));
    };
    file.write(kOverviewMagic, 4);
    write(overview.grid.originX);
    write(overview.grid.originZ);
    write(overview.grid.texelSize);
    write(overview.grid.n);
    file.write(
        reinterpret_cast<const char*>(overview.heights.data()),
        static_cast<std::streamsize>(overview.heights.size() *
                                     sizeof(f32)));
    if (!file) {
        LOG_ERROR("map bake: cannot write overview in {}",
                  run.mapDir.string());
        return false;
    }
    return true;
}

// The manifest marks the map COMPLETE and carries what the runtime
// needs before any slice is read. Written last: its absence means the
// map must be (re)baked.
bool writeManifest(MapBakeRun& run) {
    const i32 tilesPerSide = run.tilesPerSide;
    std::ofstream manifest { run.mapDir / "manifest.txt",
                             std::ios::trunc };
    manifest << "meadows-map " << kMapBakeVersion << "\n"
             << "map " << run.mapX << " " << run.mapZ << "\n"
             << "mapSize " << run.mapParams.tileSize << "\n"
             << "tileSize " << run.params.tileSize << "\n"
             << "tilesPerSide " << tilesPerSide << "\n"
             << "seed " << run.params.worldSeed << "\n"
             << "bakeVersion " << kTileBakeVersion << "\n"
             << "key " << mapBakeKey(run.params, tilesPerSide) << "\n";
    for (i32 dz = 0; dz < tilesPerSide; ++dz) {
        for (i32 dx = 0; dx < tilesPerSide; ++dx) {
            manifest << "slice " << (run.tx0 + dx) << " " << (run.tz0 + dz)
                     << "\n";
        }
    }
    if (!manifest) {
        LOG_ERROR("map bake: cannot write manifest in {}",
                  run.mapDir.string());
        return false;
    }
    LOG_INFO("map bake ({}, {}): {} slice(s) in {:.1f} s (+ {:.1f} s "
             "stage-1) -> {}",
             run.mapX, run.mapZ, run.stats.slicesWritten,
             run.stats.sliceSeconds, run.stats.stage1Seconds,
             run.mapDir.string());
    return true;
}

} // namespace

MapBakeStats bakeMap(const TileBakeParams& params, i32 mapX, i32 mapZ,
                     const std::filesystem::path& cacheDir,
                     core::JobSystem* jobs, i32 tilesPerSide,
                     const std::function<void(u32, u32)>& progress) {
    MapBakeRun run { params, mapX, mapZ, cacheDir, jobs, tilesPerSide,
                     progress };
    // The stage order IS the map pipeline's contract: a stage that
    // stops (cancelled or failed) leaves `stats` saying how far it got.
    using Stage = bool (*)(MapBakeRun&);
    constexpr Stage kStages[] = {
        createMapDir,        bakeGlobalStage1, bakeSliceGrounds,
        extractHydrology,    carveSlicesAndWrite, writeOverview,
        writeManifest,
    };
    for (const Stage stage : kStages) {
        if (!stage(run)) {
            break;
        }
    }
    return run.stats;
}

} // namespace game
