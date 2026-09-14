#include "game/MapBaker.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <thread>

#include "engine/core/ConcurrentQueue.hpp"
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

std::filesystem::path mapCacheDir(const std::filesystem::path& cacheDir,
                                  i32 mapX, i32 mapZ) {
    char dir[48];
    std::snprintf(dir, sizeof(dir), "map_%d_%d", mapX, mapZ);
    return cacheDir / dir;
}

MapBakeStats bakeMap(const TileBakeParams& params, i32 mapX, i32 mapZ,
                     const std::filesystem::path& cacheDir,
                     core::JobSystem* jobs, i32 tilesPerSide,
                     const std::function<void(u32, u32)>& progress) {
    MapBakeStats stats;
    const std::atomic<bool>* cancel = jobs ? &jobs->stopFlag() : nullptr;
    const auto cancelled = [cancel] {
        return cancel && cancel->load(std::memory_order_relaxed);
    };

    const auto mapDir = mapCacheDir(cacheDir, mapX, mapZ);
    std::error_code ec;
    std::filesystem::create_directories(mapDir, ec);
    if (ec) {
        LOG_ERROR("map bake: cannot create {} ({})", mapDir.string(),
                  ec.message());
        return stats;
    }

    // The global stage-1: same recipe, map-sized window. The apron must
    // cover the widest per-slice composite request (the canonical-basin
    // window, slice + kBasinResolveMargin) for every slice of the map.
    TileBakeParams mapParams = params;
    mapParams.tileSize =
        params.tileSize * static_cast<f32>(tilesPerSide);
    mapParams.apron = kBasinResolveMargin;
    // The caller picks the edge STYLES (params.mapEdge.valid + sides);
    // the rect always derives from the map itself.
    if (mapParams.mapEdge.valid) {
        mapParams.mapEdge.minX =
            static_cast<f32>(mapX) * mapParams.tileSize;
        mapParams.mapEdge.minZ =
            static_cast<f32>(mapZ) * mapParams.tileSize;
        mapParams.mapEdge.size = mapParams.tileSize;
        mapParams.mapEdge.seaLevel = params.macro.seaLevel;
    }
    const auto s1Start = std::chrono::steady_clock::now();
    const TileStage1 mapS1 = bakeTileStage1(mapParams, mapX, mapZ,
                                            cancel);
    stats.stage1Seconds = secondsSince(s1Start);
    if (cancelled()) {
        stats.cancelled = true;
        return stats; // partial stage-1: no slice, no manifest
    }
    LOG_INFO("map bake ({}, {}): global stage-1 {}x{} in {:.1f} s",
             mapX, mapZ, mapS1.sim.n, mapS1.sim.n, stats.stage1Seconds);

    // ONE hydrology for the whole map: every slice carves against the
    // same routed water — this, with the shared surface, is what makes
    // slice borders agree (per-slice windows carved toward different
    // water surfaces: ~100 m of band divergence measured).
    const auto hydroStart = std::chrono::steady_clock::now();
    const MapHydrology mapHydro = extractMapHydrology(
        params, mapS1, mapX, mapZ, tilesPerSide, cancel);
    if (cancelled()) {
        stats.cancelled = true;
        return stats;
    }
    LOG_INFO("map bake ({}, {}): map hydrology {}x{} in {:.1f} s — {} "
             "lake(s), {} river(s)",
             mapX, mapZ, mapHydro.window.n, mapHydro.window.n,
             secondsSince(hydroStart), mapHydro.hydro.lakes.size(),
             mapHydro.hydro.rivers.size());

    // Slices are independent given the const shared stage-1+hydrology:
    // workers write their own files, the caller only counts
    // completions (Phase-5: no shared mutable state).
    const i32 tx0 = mapX * tilesPerSide;
    const i32 tz0 = mapZ * tilesPerSide;
    const u32 total =
        static_cast<u32>(tilesPerSide) * static_cast<u32>(tilesPerSide);
    const auto s2Start = std::chrono::steady_clock::now();
    core::ConcurrentQueue<i32> done; // 1 = written, 0 = failed/cancelled
    u32 landed = 0;
    u32 written = 0;
    const auto bakeSlice = [&](i32 tx, i32 tz) {
        if (cancelled()) {
            done.push(0);
            return;
        }
        TileBakeResult slice =
            bakeMapSlice(params, tx, tz, mapS1, mapHydro, cancel);
        if (cancelled()) {
            done.push(0);
            return;
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
            const i32 tx = tx0 + dx;
            const i32 tz = tz0 + dz;
            if (jobs) {
                jobs->enqueue([&, tx, tz] { bakeSlice(tx, tz); });
            } else {
                bakeSlice(tx, tz);
            }
        }
    }
    while (landed < total) {
        i32 ok = 0;
        if (done.tryPop(ok)) {
            ++landed;
            written += ok > 0 ? 1u : 0u;
            if (progress) {
                progress(landed, total);
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    stats.sliceSeconds = secondsSince(s2Start);
    stats.slicesWritten = written;
    if (cancelled() || written != total) {
        stats.cancelled = cancelled();
        LOG_WARN("map bake ({}, {}): incomplete ({}/{} slices) — no "
                 "manifest written",
                 mapX, mapZ, written, total);
        return stats;
    }

    // The overview: the shared surface decimated to 64 m (exact texel
    // picks — rim and shaped apron included), the runtime fallback
    // inside the map.
    {
        MapOverview overview;
        overview.grid.originX = mapS1.sim.originX;
        overview.grid.originZ = mapS1.sim.originZ;
        overview.grid.texelSize =
            mapS1.sim.texelSize * static_cast<f32>(kOverviewStep);
        overview.grid.n = (mapS1.sim.n - 1) / kOverviewStep + 1;
        overview.heights.resize(overview.grid.cells());
        for (u32 row = 0; row < overview.grid.n; ++row) {
            for (u32 col = 0; col < overview.grid.n; ++col) {
                overview.heights[static_cast<size_t>(row) *
                                     overview.grid.n +
                                 col] =
                    mapS1.eroded[static_cast<size_t>(row) *
                                     kOverviewStep * mapS1.sim.n +
                                 static_cast<size_t>(col) *
                                     kOverviewStep];
            }
        }
        std::ofstream file { mapDir / "overview.bin",
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
                      mapDir.string());
            return stats;
        }
    }

    // The manifest marks the map COMPLETE and carries what the runtime
    // needs before any slice is read. Written last: its absence means
    // the map must be (re)baked.
    std::ofstream manifest { mapDir / "manifest.txt",
                             std::ios::trunc };
    manifest << "meadows-map " << kMapBakeVersion << "\n"
             << "map " << mapX << " " << mapZ << "\n"
             << "mapSize " << mapParams.tileSize << "\n"
             << "tileSize " << params.tileSize << "\n"
             << "tilesPerSide " << tilesPerSide << "\n"
             << "seed " << params.worldSeed << "\n"
             << "bakeVersion " << kTileBakeVersion << "\n";
    for (i32 dz = 0; dz < tilesPerSide; ++dz) {
        for (i32 dx = 0; dx < tilesPerSide; ++dx) {
            manifest << "slice " << (tx0 + dx) << " " << (tz0 + dz)
                     << "\n";
        }
    }
    if (!manifest) {
        LOG_ERROR("map bake: cannot write manifest in {}",
                  mapDir.string());
        return stats;
    }
    LOG_INFO("map bake ({}, {}): {} slice(s) in {:.1f} s (+ {:.1f} s "
             "stage-1) -> {}",
             mapX, mapZ, written, stats.sliceSeconds,
             stats.stage1Seconds, mapDir.string());
    return stats;
}

} // namespace game
