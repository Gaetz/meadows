#include "game/TerrainBakeStreamer.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <fstream>

#include <glm/glm.hpp>

#include "engine/core/Log.hpp"
#include "engine/terrain/generation/GridOps.hpp"
#include "game/MapBaker.hpp"
#include "world/terrain/TerrainRegions.hpp"

namespace game {

namespace {

using render::terraingen::Lake;
using render::terraingen::River;
using render::terraingen::RiverPoint;

constexpr char kWaterMagic[4] = { 'T', 'W', 'B', '7' };

} // namespace

// Water sidecar next to the tile's .trg: the lakes/rivers a re-load
// cannot re-derive without re-running the bake. Lakes carry their basin
// mask, so no fixed-size dump — per-field IO.
bool writeWaterFile(const std::filesystem::path& path,
                    const vector<Lake>& lakes,
                    const vector<River>& rivers) {
    std::ofstream file { path, std::ios::binary | std::ios::trunc };
    if (!file) {
        return false;
    }
    const auto write = [&](const auto& value) {
        file.write(reinterpret_cast<const char*>(&value), sizeof(value));
    };
    file.write(kWaterMagic, 4);
    write(static_cast<u32>(lakes.size()));
    for (const Lake& lake : lakes) {
        write(lake.level);
        write(lake.cells);
        write(lake.minX);
        write(lake.minZ);
        write(lake.maxX);
        write(lake.maxZ);
        write(lake.maskWidth);
        write(lake.maskHeight);
        write(lake.maskTexel);
        write(lake.dug);
        write(lake.catchment);
        file.write(reinterpret_cast<const char*>(lake.mask.data()),
                   static_cast<std::streamsize>(lake.mask.size()));
    }
    write(static_cast<u32>(rivers.size()));
    for (const River& river : rivers) {
        write(river.tier);
        write(river.mouthArea);
        write(river.lakeFed);
        write(river.sourceLake);
        write(river.headType);
        write(river.headIsSource);
        write(static_cast<u32>(river.fords.size()));
        for (const Vec2& ford : river.fords) {
            write(ford.x);
            write(ford.y);
        }
        write(static_cast<u32>(river.points.size()));
        file.write(reinterpret_cast<const char*>(river.points.data()),
                   static_cast<std::streamsize>(river.points.size() *
                                                sizeof(RiverPoint)));
    }
    return static_cast<bool>(file);
}

bool readWaterFile(const std::filesystem::path& path, vector<Lake>& lakes,
                   vector<River>& rivers) {
    std::ifstream file { path, std::ios::binary };
    if (!file) {
        return false;
    }
    char magic[4] = {};
    file.read(magic, 4);
    u32 lakeCount = 0;
    file.read(reinterpret_cast<char*>(&lakeCount), sizeof(lakeCount));
    if (!file || std::memcmp(magic, kWaterMagic, 4) != 0 ||
        lakeCount > 100000) {
        return false;
    }
    const auto read = [&](auto& value) {
        file.read(reinterpret_cast<char*>(&value), sizeof(value));
    };
    lakes.resize(lakeCount);
    for (Lake& lake : lakes) {
        read(lake.level);
        read(lake.cells);
        read(lake.minX);
        read(lake.minZ);
        read(lake.maxX);
        read(lake.maxZ);
        read(lake.maskWidth);
        read(lake.maskHeight);
        read(lake.maskTexel);
        read(lake.dug);
        read(lake.catchment);
        if (!file || lake.maskWidth > 100000 || lake.maskHeight > 100000) {
            return false;
        }
        lake.mask.resize(static_cast<size_t>(lake.maskWidth) *
                         lake.maskHeight);
        file.read(reinterpret_cast<char*>(lake.mask.data()),
                  static_cast<std::streamsize>(lake.mask.size()));
    }
    u32 riverCount = 0;
    file.read(reinterpret_cast<char*>(&riverCount), sizeof(riverCount));
    if (!file || riverCount > 100000) {
        return false;
    }
    rivers.resize(riverCount);
    for (River& river : rivers) {
        read(river.tier);
        read(river.mouthArea);
        read(river.lakeFed);
        read(river.sourceLake);
        read(river.headType);
        read(river.headIsSource);
        u32 fords = 0;
        file.read(reinterpret_cast<char*>(&fords), sizeof(fords));
        if (!file || fords > 100000) {
            return false;
        }
        river.fords.resize(fords);
        for (Vec2& ford : river.fords) {
            read(ford.x);
            read(ford.y);
        }
        u32 points = 0;
        file.read(reinterpret_cast<char*>(&points), sizeof(points));
        if (!file || points > 1000000) {
            return false;
        }
        river.points.resize(points);
        file.read(reinterpret_cast<char*>(river.points.data()),
                  static_cast<std::streamsize>(points *
                                               sizeof(RiverPoint)));
    }
    return static_cast<bool>(file);
}

void clipLakesToMapRect(vector<Lake>& lakes, f32 rectMinX, f32 rectMinZ,
                        f32 rectSize) {
    const f32 rectMaxX = rectMinX + rectSize;
    const f32 rectMaxZ = rectMinZ + rectSize;
    for (size_t li = 0; li < lakes.size();) {
        Lake& lake = lakes[li];
        if (lake.mask.empty() || lake.maskWidth == 0) {
            ++li; // authored maskless pond: not map-owned hydrology
            continue;
        }
        if (lake.minX >= rectMinX && lake.maxX <= rectMaxX &&
            lake.minZ >= rectMinZ && lake.maxZ <= rectMaxZ) {
            ++li;
            continue;
        }
        u32 wet = 0;
        for (u32 r = 0; r < lake.maskHeight; ++r) {
            // Masks are POINT lattices: texel m sits at min + m*texel
            // (the producers' convention) — the old +0.5 corner read
            // clipped them half a texel off the map-rect line.
            const f32 z =
                lake.minZ + static_cast<f32>(r) * lake.maskTexel;
            for (u32 c = 0; c < lake.maskWidth; ++c) {
                u8& m = lake.mask[static_cast<size_t>(r) *
                                      lake.maskWidth +
                                  c];
                if (!m) {
                    continue;
                }
                const f32 x =
                    lake.minX + static_cast<f32>(c) * lake.maskTexel;
                if (x < rectMinX || x > rectMaxX || z < rectMinZ ||
                    z > rectMaxZ) {
                    m = 0;
                    continue;
                }
                ++wet;
            }
        }
        if (wet == 0) {
            lakes.erase(lakes.begin() + static_cast<ptrdiff_t>(li));
        } else {
            ++li;
        }
    }
}

TerrainBakeStreamer::TerrainBakeStreamer(
    const render::terraingen::TileBakeParams& bakeParams,
    std::filesystem::path dir, core::JobSystem* jobSystem,
    MapStreamConfig mapConfig)
    : params { bakeParams }, cacheDir { std::move(dir) },
      jobs { jobSystem }, map { mapConfig },
      built { std::make_shared<
          core::ConcurrentQueue<PublishedTile>>() } {
    if (map.borders) {
        params.mapGrid.valid = true;
        params.mapGrid.seed = params.worldSeed;
        params.mapGrid.mapSize =
            params.tileSize * static_cast<f32>(map.tilesPerSide);
        params.mapGrid.seaLevel = params.macro.seaLevel;
    }
    std::error_code ec;
    std::filesystem::create_directories(cacheDir, ec);
    if (ec) {
        LOG_WARN("Terrain cache: cannot create {} ({})",
                 cacheDir.string(), ec.message());
    }
}

void TerrainBakeStreamer::request(i32 tx, i32 tz) {
    // Slices come from the map cache; a request into an unbaked map
    // defers the tile and kicks ONE background map bake.
    const i32 tps = map.tilesPerSide;
        const auto floorDiv = [](i32 a, i32 b) {
            return a >= 0 ? a / b : -((-a + b - 1) / b);
        };
        if (floorDiv(tx, tps) != map.mapX ||
            floorDiv(tz, tps) != map.mapZ) {
            return; // beyond the map rect: nothing exists there
        }
        if (deferredForMap.count(keyOf(tx, tz))) {
            return; // already parked on the map bake — no fs churn
        }
        const auto mapDir = mapCacheDir(cacheDir, map.mapX, map.mapZ);
        if (!mapBakedAndValid(cacheDir, map.mapX, map.mapZ,
                              map.tilesPerSide, &params)) {
            deferredForMap.insert(keyOf(tx, tz));
            if (!mapBaking->exchange(true)) {
                const auto work = [params = params, cacheDir = cacheDir,
                                   mx = map.mapX, mz = map.mapZ,
                                   tps = tps, jobsRef = jobs,
                                   baking = mapBaking] {
                    if (!jobsRef || !jobsRef->isStopping()) {
                        bakeMap(params, mx, mz, cacheDir, jobsRef, tps);
                    }
                    baking->store(false);
                };
                if (jobs) {
                    jobs->enqueue(work);
                } else {
                    work();
                }
            }
            return;
        }
        if (!mapLakesRequested) {
            // First proof the map cache is whole: read the map-wide
            // lake registry (headers + masks of every slice's .twb,
            // rivers skipped) on a worker. See takeMapLakes().
            mapLakesRequested = true;
            const auto scan = [mapDir, tps, mx = map.mapX,
                               mz = map.mapZ, queue = mapLakesBuilt,
                               jobsRef = jobs,
                               tileSize = params.tileSize] {
                if (jobsRef && jobsRef->isStopping()) {
                    return;
                }
                vector<render::terraingen::Lake> all;
                for (i32 sz = mz * tps; sz < (mz + 1) * tps; ++sz) {
                    for (i32 sx = mx * tps; sx < (mx + 1) * tps;
                         ++sx) {
                        const std::string stem =
                            "tile_" + std::to_string(sx) + "_" +
                            std::to_string(sz) + "_v" +
                            std::to_string(
                                render::terraingen::kTileBakeVersion);
                        vector<render::terraingen::Lake> lakes;
                        vector<render::terraingen::River> rivers;
                        if (readWaterFile(mapDir / (stem + ".twb"),
                                          lakes, rivers)) {
                            all.insert(
                                all.end(),
                                std::make_move_iterator(lakes.begin()),
                                std::make_move_iterator(lakes.end()));
                        }
                    }
                }
                const f32 mapSize = tileSize * static_cast<f32>(tps);
                clipLakesToMapRect(all,
                                   static_cast<f32>(mx) * mapSize,
                                   static_cast<f32>(mz) * mapSize,
                                   mapSize);
                queue->push(std::move(all));
            };
            if (jobs) {
                jobs->enqueue(scan);
            } else {
                scan();
            }
        }
        pending.insert(keyOf(tx, tz));
        const f32 mapSizeOf = params.tileSize * static_cast<f32>(tps);
        const auto work = [mapDir, tx, tz, queue = built,
                           jobsRef = jobs,
                           mapMinX = static_cast<f32>(map.mapX) *
                                     mapSizeOf,
                           mapMinZ = static_cast<f32>(map.mapZ) *
                                     mapSizeOf,
                           mapSizeOf,
                           detailAmp =
                               render::terraingen::kRegionDetailAmplitude,
                           detailWave =
                               render::terraingen::kRegionDetailWavelength,
                           detailOct =
                               render::terraingen::kRegionDetailOctaves] {
            if (jobsRef && jobsRef->isStopping()) {
                return;
            }
            const std::string stem =
                "tile_" + std::to_string(tx) + "_" + std::to_string(tz) +
                "_v" +
                std::to_string(render::terraingen::kTileBakeVersion);
            PublishedTile tile;
            tile.tx = tx;
            tile.tz = tz;
            auto slice = world::readTrgFile(mapDir / (stem + ".trg"));
            if (!slice ||
                !readWaterFile(mapDir / (stem + ".twb"), tile.lakes,
                               tile.rivers)) {
                LOG_ERROR("Map cache: slice ({}, {}) unreadable in {} — "
                          "delete the map dir to force a re-bake",
                          tx, tz, mapDir.string());
                return;
            }
            clipLakesToMapRect(tile.lakes, mapMinX, mapMinZ, mapSizeOf);
            tile.region = std::move(*slice);
            tile.region.detailAmplitude = detailAmp;
            tile.region.detailWavelength = detailWave;
            tile.region.detailOctaves = detailOct;
            queue->push(std::move(tile));
        };
        if (jobs) {
            jobs->enqueue(work);
        } else {
            work();
        }
}

void TerrainBakeStreamer::prefetchMap(i32 mapX, i32 mapZ) {
    if (mapBakedAndValid(cacheDir, mapX, mapZ, map.tilesPerSide,
                         &params)) {
        return;
    }
    if (mapBaking->exchange(true)) {
        return; // one map bake at a time, ever
    }
    LOG_INFO("Map prefetch: baking neighbour map ({}, {}) in the "
             "background",
             mapX, mapZ);
    const auto work = [params = params, cacheDir = cacheDir, mapX,
                       mapZ, tps = map.tilesPerSide, jobsRef = jobs,
                       baking = mapBaking] {
        if (!jobsRef || !jobsRef->isStopping()) {
            bakeMap(params, mapX, mapZ, cacheDir, jobsRef, tps);
        }
        baking->store(false);
    };
    if (jobs) {
        jobs->enqueue(work);
    } else {
        work();
    }
}

TerrainBakeStreamer::RingStatus TerrainBakeStreamer::ringStatus(
    const Vec3& focus) const {
    const f32 t = params.tileSize;
    const i32 tx0 =
        static_cast<i32>(std::floor((focus.x - prefetchReach) / t));
    const i32 tx1 =
        static_cast<i32>(std::floor((focus.x + prefetchReach) / t));
    const i32 tz0 =
        static_cast<i32>(std::floor((focus.z - prefetchReach) / t));
    const i32 tz1 =
        static_cast<i32>(std::floor((focus.z + prefetchReach) / t));
    RingStatus status;
    for (i32 tz = tz0; tz <= tz1; ++tz) {
        for (i32 tx = tx0; tx <= tx1; ++tx) {
            // Only in-rect tiles count: beyond the rim nothing
            // exists, and counting it would hold the warmup gate
            // open forever near a map edge.
            const i32 tps = map.tilesPerSide;
            const auto floorDiv = [](i32 a, i32 b) {
                return a >= 0 ? a / b : -((-a + b - 1) / b);
            };
            if (floorDiv(tx, tps) != map.mapX ||
                floorDiv(tz, tps) != map.mapZ) {
                continue;
            }
            ++status.needed;
            if (published.count(keyOf(tx, tz))) {
                ++status.published;
            }
        }
    }
    return status;
}

void TerrainBakeStreamer::update(
    const Vec3& focus,
    const std::function<void(PublishedTile&&)>& publish) {
    // Desired set: every tile whose rect intersects the prefetch
    // square, requested HEADING-FIRST — the bake wavefront leads the
    // movement instead of filling the square in scan order.
    const f32 t = params.tileSize;
    const i32 tx0 =
        static_cast<i32>(std::floor((focus.x - prefetchReach) / t));
    const i32 tx1 =
        static_cast<i32>(std::floor((focus.x + prefetchReach) / t));
    const i32 tz0 =
        static_cast<i32>(std::floor((focus.z - prefetchReach) / t));
    const i32 tz1 =
        static_cast<i32>(std::floor((focus.z + prefetchReach) / t));
    Vec2 heading { 0.0f, 0.0f };
    {
        const Vec2 moved { focus.x - lastFocus.x, focus.z - lastFocus.z };
        const f32 len = glm::length(moved);
        if (len > 0.5f) {
            heading = moved / len;
        }
        lastFocus = focus;
    }
    struct Want {
        f32 score;
        i32 tx;
        i32 tz;
    };
    vector<Want> wanted;
    for (i32 tz = tz0; tz <= tz1; ++tz) {
        for (i32 tx = tx0; tx <= tx1; ++tx) {
            const u64 key = keyOf(tx, tz);
            if (published.count(key) || pending.count(key)) {
                continue;
            }
            const Vec2 delta {
                (static_cast<f32>(tx) + 0.5f) * t - focus.x,
                (static_cast<f32>(tz) + 0.5f) * t - focus.z
            };
            const f32 dist = glm::length(delta);
            const f32 ahead =
                dist > 1.0f ? glm::dot(delta / dist, heading) : 0.0f;
            wanted.push_back({ dist * (1.1f - 0.4f * ahead), tx, tz });
        }
    }
    std::sort(wanted.begin(), wanted.end(),
              [](const Want& a, const Want& b) {
                  return a.score < b.score;
              });
    for (const Want& want : wanted) {
        request(want.tx, want.tz);
    }
    // Deferred map tiles: once the background map bake lands its
    // manifest, re-drive them through the read path (throttled — an
    // exists() per frame per tile would be waste).
    if (!deferredForMap.empty() && !mapBaking->load()) {
        if (manifestCheckCountdown > 0) {
            --manifestCheckCountdown;
        } else {
            manifestCheckCountdown = 30;
            if (mapBakedAndValid(cacheDir, map.mapX, map.mapZ,
                                 map.tilesPerSide, &params)) {
                const auto deferred = std::move(deferredForMap);
                deferredForMap.clear();
                for (const u64 key : deferred) {
                    request(static_cast<i32>(
                                static_cast<u32>(key >> 32)),
                            static_cast<i32>(
                                static_cast<u32>(key & 0xFFFFFFFFull)));
                }
            }
        }
    }
    // Drain the mailbox on the frame thread.
    drain(publish);
}

void TerrainBakeStreamer::drain(
    const std::function<void(PublishedTile&&)>& publish) {
    PublishedTile tile;
    while (built->tryPop(tile)) {
        const u64 key = keyOf(tile.tx, tile.tz);
        pending.erase(key);
        published.insert(key);
        publish(std::move(tile));
        tile = PublishedTile {};
    }
}

bool farWaterWetAt(const render::WaterSystem::FarWaterSet& set, f32 x,
                   f32 z) {
    for (const render::WaterSystem::FarWaterSet::Lake& lake : set.lakes) {
        const f32 fx = (x - lake.minX) / lake.cell;
        const f32 fz = (z - lake.minZ) / lake.cell;
        if (fx < 0.0f || fz < 0.0f) {
            continue;
        }
        const u32 c = static_cast<u32>(fx);
        const u32 r = static_cast<u32>(fz);
        if (c < lake.w && r < lake.h &&
            lake.mask[static_cast<size_t>(r) * lake.w + c]) {
            return true;
        }
    }
    for (const render::WaterSystem::FarWaterSet::Ribbon& ribbon :
         set.ribbons) {
        for (size_t i = 1; i < ribbon.nodes.size(); ++i) {
            const auto& a = ribbon.nodes[i - 1];
            const auto& b = ribbon.nodes[i];
            const Vec2 ab { b.x - a.x, b.z - a.z };
            const Vec2 ap { x - a.x, z - a.z };
            const f32 len2 = glm::dot(ab, ab);
            const f32 t = len2 > 0.0f
                              ? glm::clamp(glm::dot(ap, ab) / len2, 0.0f, 1.0f)
                              : 0.0f;
            const Vec2 d = ap - ab * t;
            const f32 halfWidth = glm::mix(a.halfWidth, b.halfWidth, t);
            if (glm::dot(d, d) <= halfWidth * halfWidth) {
                return true;
            }
        }
    }
    return false;
}

render::WaterSystem::FarWaterSet collectFarWater(
    const std::filesystem::path& cacheRoot, i32 mapX, i32 mapZ,
    f32 tileSize,
    const render::terraingen::ProceduralControlParams& controls,
    const render::terraingen::MacroParams& macro,
    const render::terraingen::MasterNetworkParams& net,
    const render::terraingen::MapGridSpec& grid,
    const render::terraingen::RiverWidthLaw& law, f32 pinHalfWidth,
    f32 seaLevel, f32 cx, f32 cz, f32 halfSpan) {
    render::WaterSystem::FarWaterSet set;
    const f32 minX = cx - halfSpan;
    const f32 maxX = cx + halfSpan;
    const f32 minZ = cz - halfSpan;
    const f32 maxZ = cz + halfSpan;
    struct Rect {
        f32 x0, z0, x1, z1;
    };
    vector<Rect> covered;
    // The active map's slice dir and its cached neighbours' (the
    // square reaches into them at 8 km maps); a missing dir is skipped.
    // Each dir carries its map rect: the lakes it holds are clipped to
    // it (clipLakesToMapRect — one hydrology authority per point).
    struct MapDir {
        std::filesystem::path path;
        f32 minX, minZ;
    };
    const f32 mapSize = tileSize * static_cast<f32>(kMapTilesPerSide);
    vector<MapDir> dirs;
    for (i32 dz = -1; dz <= 1; ++dz) {
        for (i32 dx = -1; dx <= 1; ++dx) {
            dirs.push_back(
                { mapCacheDir(cacheRoot, mapX + dx, mapZ + dz),
                  static_cast<f32>(mapX + dx) * mapSize,
                  static_cast<f32>(mapZ + dz) * mapSize });
        }
    }
    for (const MapDir& mapDirEntry : dirs) {
    const std::filesystem::path& cacheDir = mapDirEntry.path;
    // The far ribbons are depth-tested against the FarTerrain built
    // from this dir's overview: a published surface seated on the 2 m
    // carved floor dips under the 64 m picks about half the time
    // (lattice aliasing across a gorge), shredding the ribbon into
    // patches from afar. Clamp each FAR node to the overview ground —
    // a display-resolution reconciliation, invisible at kilometres,
    // NOT the per-texel near lift (removed; the near mesh and sim own
    // the close field).
    const auto farOverview = loadMapOverview(cacheDir);
    const auto farGroundLift = [&](f32 x, f32 z, f32 surface) {
        if (!farOverview) {
            return surface;
        }
        const render::terraingen::GridSpec& g = farOverview->grid;
        const f32 gMaxX =
            g.originX + static_cast<f32>(g.n - 1) * g.texelSize;
        const f32 gMaxZ =
            g.originZ + static_cast<f32>(g.n - 1) * g.texelSize;
        if (x < g.originX || x > gMaxX || z < g.originZ ||
            z > gMaxZ) {
            return surface;
        }
        // Capped: in a gorge the 64 m overview averages the rims in
        // — an unbounded lift hung far ribbons metres in the air (the
        // floating "tongue", measured dev). Past the cap the far
        // ribbon stays buried; the near field owns it on approach.
        return glm::min(
            glm::max(surface,
                     render::terraingen::bilinearWorld(
                         g, farOverview->heights, x, z) +
                         0.4f),
            surface + 6.0f);
    };
    std::error_code ec;
    for (std::filesystem::directory_iterator it { cacheDir, ec }, end;
         !ec && it != end; it.increment(ec)) {
        const std::filesystem::path& path = it->path();
        if (path.extension() != ".twb") {
            continue;
        }
        i32 tx = 0;
        i32 tz = 0;
        u32 ver = 0;
        if (std::sscanf(path.filename().string().c_str(),
                        "tile_%d_%d_v%u", &tx, &tz, &ver) != 3 ||
            ver != render::terraingen::kTileBakeVersion) {
            continue;
        }
        const Rect rect { static_cast<f32>(tx) * tileSize,
                          static_cast<f32>(tz) * tileSize,
                          static_cast<f32>(tx + 1) * tileSize,
                          static_cast<f32>(tz + 1) * tileSize };
        if (rect.x1 < minX || rect.x0 > maxX || rect.z1 < minZ ||
            rect.z0 > maxZ) {
            continue;
        }
        vector<Lake> lakes;
        vector<River> rivers;
        if (!readWaterFile(path, lakes, rivers)) {
            continue;
        }
        clipLakesToMapRect(lakes, mapDirEntry.minX, mapDirEntry.minZ,
                           mapSize);
        // Baked tiles own their footprint even when read failed lakes
        // only after this point — the master fallback must never
        // duplicate a course a real bake already carved.
        covered.push_back(rect);
        for (const Lake& lake : lakes) {
            if (lake.level <= seaLevel + 0.25f || lake.mask.empty() ||
                lake.maskWidth == 0) {
                continue;
            }
            const u32 k = glm::max(
                1u, static_cast<u32>(
                        std::lround(64.0f / lake.maskTexel)));
            render::WaterSystem::FarWaterSet::Lake far;
            far.level = lake.level;
            far.minX = lake.minX;
            far.minZ = lake.minZ;
            far.cell = lake.maskTexel * static_cast<f32>(k);
            far.w = (lake.maskWidth + k - 1) / k;
            far.h = (lake.maskHeight + k - 1) / k;
            far.mask.assign(static_cast<size_t>(far.w) * far.h, 0);
            for (u32 r = 0; r < far.h; ++r) {
                for (u32 c = 0; c < far.w; ++c) {
                    u32 hits = 0;
                    u32 total = 0;
                    for (u32 sr = r * k;
                         sr < glm::min((r + 1) * k, lake.maskHeight);
                         ++sr) {
                        for (u32 sc = c * k;
                             sc <
                             glm::min((c + 1) * k, lake.maskWidth);
                             ++sc) {
                            ++total;
                            hits += lake.mask[static_cast<size_t>(sr) *
                                                  lake.maskWidth +
                                              sc]
                                        ? 1u
                                        : 0u;
                        }
                    }
                    // ANY wet texel marks the block: a discovered
                    // pool 1-3 texels wide never wins a majority of
                    // an 8x8 block — whole pools vanished from the
                    // far view while their ribbon is cut underneath
                    // (no far water at all over them, measured dev).
                    // The depth test against the FarTerrain cuts the
                    // overdraw at the banks, as it already does for
                    // every far sheet.
                    far.mask[static_cast<size_t>(r) * far.w + c] =
                        (total != 0 && hits != 0) ? 1 : 0;
                }
            }
            // A narrow sheet can still sit under the 64 m overview's
            // bilinear banks (the same aliasing the ribbon clamp
            // handles): lift a BURIED sheet just enough to graze the
            // far ground at its lowest covered block, bounded — a
            // sub-resolution pool stays hidden rather than floating.
            if (farOverview) {
                f32 minG = 1.0e18f;
                for (u32 r = 0; r < far.h; ++r) {
                    for (u32 c = 0; c < far.w; ++c) {
                        if (!far.mask[static_cast<size_t>(r) * far.w +
                                      c]) {
                            continue;
                        }
                        const f32 bx = far.minX +
                                       (static_cast<f32>(c) + 0.5f) *
                                           far.cell;
                        const f32 bz = far.minZ +
                                       (static_cast<f32>(r) + 0.5f) *
                                           far.cell;
                        minG = glm::min(
                            minG, farGroundLift(bx, bz, -1.0e18f));
                    }
                }
                if (minG < 1.0e17f && far.level < minG) {
                    far.level =
                        glm::min(minG, far.level + 4.0f);
                }
            }
            set.lakes.push_back(std::move(far));
        }
        for (const River& river : rivers) {
            render::WaterSystem::FarWaterSet::Ribbon run;
            // The decimation must not eat a run's TERMINAL node: the
            // last point is the one overlap point under a lake sheet
            // or the mouth itself — dropping it left up to 48 m of
            // dry far gap at every inlet (measured dev).
            const RiverPoint* pending = nullptr;
            const auto push = [&](const RiverPoint& pt) {
                run.nodes.push_back(
                    { pt.x, pt.z,
                      farGroundLift(pt.x, pt.z, pt.surface),
                      pt.halfWidth });
            };
            const auto flush = [&] {
                if (pending && !run.nodes.empty()) {
                    push(*pending);
                }
                pending = nullptr;
                if (run.nodes.size() >= 2) {
                    set.ribbons.push_back(std::move(run));
                }
                run = {};
            };
            for (const RiverPoint& pt : river.points) {
                // Only courses wide enough to read at kilometres —
                // the SAME threshold the sim pins at, so what the far
                // shows is exactly what the near will own; the sea
                // sheet covers estuaries. The brooks below it render
                // through the local mesh at their honest lift.
                if (pt.halfWidth < pinHalfWidth ||
                    pt.surface <= seaLevel + 0.25f) {
                    flush();
                    continue;
                }
                if (!run.nodes.empty()) {
                    const auto& last = run.nodes.back();
                    const f32 dx = pt.x - last.x;
                    const f32 dz = pt.z - last.z;
                    // A STEEP reach keeps its nodes: thinned to 48 m,
                    // a 10 m cascade smeared into a long far ramp
                    // (the near view shows a fall, the far view a
                    // slide).
                    if (dx * dx + dz * dz < 48.0f * 48.0f &&
                        last.surface - pt.surface <= 2.0f) {
                        pending = &pt; // kept if the run ends here
                        continue;      // ~48 m is plenty far away
                    }
                }
                pending = nullptr;
                push(pt);
            }
            flush();
        }
    }
    } // neighbour map dirs
    // Master fleuves wherever nothing was ever baked: the imprint (S1)
    // carved their valleys into the analytic ground the FarTerrain
    // shows, so the routed surfaces sit plausibly in them.
    const auto inCovered = [&](f32 x, f32 z) {
        for (const Rect& r : covered) {
            if (x >= r.x0 && x <= r.x1 && z >= r.z0 && z <= r.z1) {
                return true;
            }
        }
        return false;
    };
    const render::terraingen::ProceduralControls ctl { controls };
    const auto masters = render::terraingen::masterRiversNear(
        ctl, macro, net, minX, minZ, maxX, maxZ);
    for (const render::terraingen::MasterRiver& river : masters) {
        render::WaterSystem::FarWaterSet::Ribbon run;
        for (const render::terraingen::MasterNode& node : river.nodes) {
            // The course was routed on the ANALYTIC ground; where a
            // border transition reshaped it (a sea arm drowned it, a
            // range buried it), the ribbon must stop — otherwise it
            // floats over the drowned channel the far terrain shows.
            const f32 shaped = render::terraingen::applyMapGridShape(
                ctl, macro, grid, node.x, node.z, node.surface);
            if (node.surface <= seaLevel + 0.5f ||
                shaped <= seaLevel + 0.5f ||
                shaped > node.surface + 30.0f ||
                inCovered(node.x, node.z)) {
                if (run.nodes.size() >= 2) {
                    set.ribbons.push_back(std::move(run));
                }
                run = {};
                continue;
            }
            // The shared width law on the effective discharge, so
            // the far ribbon and the baked one agree at the frontier
            // — no floor: a course lies about nothing (the extraction
            // threshold already keeps master courses ~10 m wide).
            const f32 hw =
                render::terraingen::riverLawHalfWidth(law, node.qArea);
            run.nodes.push_back({ node.x, node.z, node.surface, hw });
        }
        if (run.nodes.size() >= 2) {
            set.ribbons.push_back(std::move(run));
        }
    }
    return set;
}

} // namespace game
