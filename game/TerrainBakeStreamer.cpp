#include "game/TerrainBakeStreamer.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <fstream>

#include <glm/glm.hpp>

#include "engine/core/Log.hpp"
#include "world/terrain/TerrainRegions.hpp"

namespace game {

namespace {

using render::terraingen::Lake;
using render::terraingen::River;
using render::terraingen::RiverPoint;

constexpr char kWaterMagic[4] = { 'T', 'W', 'B', '3' };

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
        file.write(reinterpret_cast<const char*>(lake.mask.data()),
                   static_cast<std::streamsize>(lake.mask.size()));
    }
    write(static_cast<u32>(rivers.size()));
    for (const River& river : rivers) {
        write(river.tier);
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

// Stage-1 cache: the per-tile eroded coarse terrain the stage-2 water
// pass composes across neighbourhoods. "TS16": spec + eroded + uplift
// + deposit + seaDist + biome + gentle + calm + trunk.
constexpr char kStage1Magic[4] = { 'T', 'S', '1', '6' };

bool writeStage1File(const std::filesystem::path& path,
                     const render::terraingen::TileStage1& s1) {
    std::ofstream file { path, std::ios::binary | std::ios::trunc };
    if (!file) {
        return false;
    }
    const auto write = [&](const auto& value) {
        file.write(reinterpret_cast<const char*>(&value), sizeof(value));
    };
    file.write(kStage1Magic, 4);
    write(s1.sim.originX);
    write(s1.sim.originZ);
    write(s1.sim.texelSize);
    write(s1.sim.n);
    file.write(reinterpret_cast<const char*>(s1.eroded.data()),
               static_cast<std::streamsize>(s1.eroded.size() *
                                            sizeof(f32)));
    file.write(reinterpret_cast<const char*>(s1.uplift.data()),
               static_cast<std::streamsize>(s1.uplift.size() *
                                            sizeof(f32)));
    file.write(reinterpret_cast<const char*>(s1.deposit.data()),
               static_cast<std::streamsize>(s1.deposit.size() *
                                            sizeof(f32)));
    file.write(reinterpret_cast<const char*>(s1.seaDist.data()),
               static_cast<std::streamsize>(s1.seaDist.size() *
                                            sizeof(f32)));
    file.write(reinterpret_cast<const char*>(s1.biome.data()),
               static_cast<std::streamsize>(s1.biome.size()));
    file.write(reinterpret_cast<const char*>(s1.gentle.data()),
               static_cast<std::streamsize>(s1.gentle.size() *
                                            sizeof(f32)));
    file.write(reinterpret_cast<const char*>(s1.calm.data()),
               static_cast<std::streamsize>(s1.calm.size() *
                                            sizeof(f32)));
    file.write(reinterpret_cast<const char*>(s1.trunk.data()),
               static_cast<std::streamsize>(s1.trunk.size() *
                                            sizeof(f32)));
    return static_cast<bool>(file);
}

std::optional<render::terraingen::TileStage1> readStage1File(
    const std::filesystem::path& path) {
    std::ifstream file { path, std::ios::binary };
    if (!file) {
        return std::nullopt;
    }
    char magic[4] = {};
    render::terraingen::TileStage1 s1;
    const auto read = [&](auto& value) {
        file.read(reinterpret_cast<char*>(&value), sizeof(value));
    };
    file.read(magic, 4);
    read(s1.sim.originX);
    read(s1.sim.originZ);
    read(s1.sim.texelSize);
    read(s1.sim.n);
    if (!file || std::memcmp(magic, kStage1Magic, 4) != 0 ||
        s1.sim.n < 2 || s1.sim.n > 8192) {
        return std::nullopt;
    }
    const size_t cells = s1.sim.cells();
    s1.eroded.resize(cells);
    s1.uplift.resize(cells);
    s1.deposit.resize(cells);
    s1.seaDist.resize(cells);
    s1.biome.resize(cells);
    s1.gentle.resize(cells);
    s1.calm.resize(cells);
    s1.trunk.resize(cells);
    file.read(reinterpret_cast<char*>(s1.eroded.data()),
              static_cast<std::streamsize>(cells * sizeof(f32)));
    file.read(reinterpret_cast<char*>(s1.uplift.data()),
              static_cast<std::streamsize>(cells * sizeof(f32)));
    file.read(reinterpret_cast<char*>(s1.deposit.data()),
              static_cast<std::streamsize>(cells * sizeof(f32)));
    file.read(reinterpret_cast<char*>(s1.seaDist.data()),
              static_cast<std::streamsize>(cells * sizeof(f32)));
    file.read(reinterpret_cast<char*>(s1.biome.data()),
              static_cast<std::streamsize>(cells));
    file.read(reinterpret_cast<char*>(s1.gentle.data()),
              static_cast<std::streamsize>(cells * sizeof(f32)));
    file.read(reinterpret_cast<char*>(s1.calm.data()),
              static_cast<std::streamsize>(cells * sizeof(f32)));
    file.read(reinterpret_cast<char*>(s1.trunk.data()),
              static_cast<std::streamsize>(cells * sizeof(f32)));
    if (!file) {
        return std::nullopt;
    }
    return s1;
}

// Load-or-bake a stage-1 (disk-cache core, no dedup).
render::terraingen::TileStage1 ensureStage1(
    const std::filesystem::path& cacheDir,
    const render::terraingen::TileBakeParams& params, i32 tx, i32 tz,
    const std::atomic<bool>* cancel) {
    const std::string stem =
        "s1_" + std::to_string(tx) + "_" + std::to_string(tz) + "_v" +
        std::to_string(render::terraingen::kStage1Version) + ".bin";
    const auto path = cacheDir / stem;
    std::error_code probe;
    if (std::filesystem::exists(path, probe)) {
        if (auto cached = readStage1File(path)) {
            return std::move(*cached);
        }
        LOG_WARN("Terrain cache: rejected {} (corrupt), rebaking",
                 path.string());
    }
    render::terraingen::TileStage1 s1 =
        render::terraingen::bakeTileStage1(params, tx, tz, cancel);
    if (cancel && cancel->load(std::memory_order_relaxed)) {
        return s1; // PARTIAL (shutdown) — never cache it to disk
    }
    if (!writeStage1File(path, s1)) {
        LOG_WARN("Terrain cache: cannot write {}", path.string());
    }
    return s1;
}

// Registry front: one worker computes a given stage-1, concurrent
// requesters WAIT for it instead of duplicating minutes of erosion.
sptr<const render::terraingen::TileStage1> acquireStage1(
    TerrainBakeStreamer::Stage1Registry& registry,
    const std::filesystem::path& cacheDir,
    const render::terraingen::TileBakeParams& params, i32 tx, i32 tz,
    const std::atomic<bool>* cancel) {
    const u64 key = (static_cast<u64>(static_cast<u32>(tx)) << 32) |
                    static_cast<u64>(static_cast<u32>(tz));
    {
        std::unique_lock lock { registry.mutex };
        for (;;) {
            const auto it = registry.done.find(key);
            if (it != registry.done.end()) {
                return it->second;
            }
            if (!registry.inflight.count(key)) {
                break;
            }
            registry.ready.wait(lock);
        }
        registry.inflight.insert(key);
    }
    auto s1 = std::make_shared<render::terraingen::TileStage1>(
        ensureStage1(cacheDir, params, tx, tz, cancel));
    if (cancel && cancel->load(std::memory_order_relaxed)) {
        // Shutdown mid-bake: the stage-1 is PARTIAL. Release the
        // inflight key and WAKE the waiters regardless — a worker
        // parked on registry.ready with no computing owner would hang
        // the JobSystem join forever — but never publish the partial
        // into `done` (a later requester would compose bad terrain).
        {
            std::lock_guard lock { registry.mutex };
            registry.inflight.erase(key);
        }
        registry.ready.notify_all();
        return s1;
    }
    registry.completed.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard lock { registry.mutex };
        // Bounded residency: the disk cache makes eviction cheap.
        if (registry.done.size() > 12) {
            registry.done.clear();
        }
        registry.done.emplace(key, s1);
        registry.inflight.erase(key);
    }
    registry.ready.notify_all();
    return s1;
}

} // namespace

TerrainBakeStreamer::TerrainBakeStreamer(
    const render::terraingen::TileBakeParams& bakeParams,
    std::filesystem::path dir, core::JobSystem* jobSystem)
    : params { bakeParams }, cacheDir { std::move(dir) },
      jobs { jobSystem },
      built { std::make_shared<
          core::ConcurrentQueue<PublishedTile>>() } {
    std::error_code ec;
    std::filesystem::create_directories(cacheDir, ec);
    if (ec) {
        LOG_WARN("Terrain cache: cannot create {} ({})",
                 cacheDir.string(), ec.message());
    }
}

u32 TerrainBakeStreamer::stage1Count() const {
    return stage1s->completed.load(std::memory_order_relaxed);
}

void TerrainBakeStreamer::request(i32 tx, i32 tz) {
    pending.insert(keyOf(tx, tz));
    const auto work = [params = params, cacheDir = cacheDir, tx, tz,
                       queue = built, registry = stage1s,
                       jobsRef = jobs] {
        // Shutdown drains the job queue (a queued save must land) —
        // an abandonable 20-40 s bake must NOT run behind a closed
        // window: bail before the heavy stages. The tile simply
        // rebakes next launch.
        if (jobsRef && jobsRef->isStopping()) {
            return;
        }
        // Cooperative cancellation INSIDE the long kernels too: the
        // erosion passes poll this between iterations, so a bake
        // already minutes deep still aborts within one iteration of
        // the quit instead of running to completion.
        const std::atomic<bool>* cancel =
            jobsRef ? &jobsRef->stopFlag() : nullptr;
        const std::string stem =
            "tile_" + std::to_string(tx) + "_" + std::to_string(tz) +
            "_v" +
            std::to_string(render::terraingen::kTileBakeVersion);
        const auto trgPath = cacheDir / (stem + ".trg");
        const auto waterPath = cacheDir / (stem + ".twb");
        PublishedTile tile;
        tile.tx = tx;
        tile.tz = tz;
        // Probe existence first: a cache miss is the normal first-visit
        // path, not a read error worth logging.
        std::error_code probe;
        const bool trgExists = std::filesystem::exists(trgPath, probe);
        auto cached =
            trgExists ? world::readTrgFile(trgPath) : std::nullopt;
        if (trgExists && !cached) {
            LOG_WARN("Terrain cache: rejected {} (corrupt), rebaking",
                     trgPath.string());
        }
        if (cached &&
            !readWaterFile(waterPath, tile.lakes, tile.rivers)) {
            LOG_WARN("Terrain cache: rejected {} (water sidecar), "
                     "rebaking",
                     waterPath.string());
            cached.reset();
            tile.lakes.clear();
            tile.rivers.clear();
        }
        if (cached) {
            tile.region = std::move(*cached);
            // Detail knobs are not in the asset: re-stamp the bake's
            // (kRegionDetail* — the single definition in TileBake.hpp).
            tile.region.detailAmplitude =
                render::terraingen::kRegionDetailAmplitude;
            tile.region.detailWavelength =
                render::terraingen::kRegionDetailWavelength;
            tile.region.detailOctaves =
                render::terraingen::kRegionDetailOctaves;
        } else {
            // Two-stage bake: gather (dedup'd across workers) the 3x3
            // stage-1 terrains, then derive the water from their
            // COMPOSITE — neighbours agree in the shared bands by
            // construction.
            sptr<const render::terraingen::TileStage1> stage1s[3][3];
            for (i32 dz = -1; dz <= 1; ++dz) {
                for (i32 dx = -1; dx <= 1; ++dx) {
                    if (jobsRef && jobsRef->isStopping()) {
                        return; // mid-bake quit checkpoint
                    }
                    stage1s[dz + 1][dx + 1] =
                        acquireStage1(*registry, cacheDir, params,
                                      tx + dx, tz + dz, cancel);
                }
            }
            if (jobsRef && jobsRef->isStopping()) {
                return;
            }
            render::terraingen::TileBakeResult baked =
                render::terraingen::bakeTileStage2(
                    params, tx, tz,
                    [&](i32 qx, i32 qz)
                        -> const render::terraingen::TileStage1* {
                        const i32 dx = qx - tx;
                        const i32 dz = qz - tz;
                        if (dx < -1 || dx > 1 || dz < -1 || dz > 1) {
                            return nullptr;
                        }
                        return stage1s[dz + 1][dx + 1].get();
                    },
                    cancel);
            if (jobsRef && jobsRef->isStopping()) {
                return; // partial stage-2: never cache, never publish
            }
            tile.region = std::move(baked.region);
            tile.lakes = std::move(baked.lakes);
            tile.rivers = std::move(baked.rivers);
            if (!world::writeTrgFile(trgPath, tile.region) ||
                !writeWaterFile(waterPath, tile.lakes, tile.rivers)) {
                LOG_WARN("Terrain cache: cannot write {}",
                         trgPath.string());
            }
        }
        queue->push(std::move(tile));
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
    // Drain the mailbox on the frame thread.
    PublishedTile tile;
    while (built->tryPop(tile)) {
        const u64 key = keyOf(tile.tx, tile.tz);
        pending.erase(key);
        published.insert(key);
        publish(std::move(tile));
        tile = PublishedTile {};
    }
}

render::WaterSystem::FarWaterSet collectFarWater(
    const std::filesystem::path& cacheDir, f32 tileSize,
    const render::terraingen::ProceduralControlParams& controls,
    const render::terraingen::MacroParams& macro,
    const render::terraingen::MasterNetworkParams& net, f32 seaLevel,
    f32 cx, f32 cz, f32 halfSpan) {
    render::WaterSystem::FarWaterSet set;
    const f32 minX = cx - halfSpan;
    const f32 maxX = cx + halfSpan;
    const f32 minZ = cz - halfSpan;
    const f32 maxZ = cz + halfSpan;
    struct Rect {
        f32 x0, z0, x1, z1;
    };
    vector<Rect> covered;
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
                    far.mask[static_cast<size_t>(r) * far.w + c] =
                        (total != 0 && hits * 2 >= total) ? 1 : 0;
                }
            }
            set.lakes.push_back(std::move(far));
        }
        for (const River& river : rivers) {
            render::WaterSystem::FarWaterSet::Ribbon run;
            const auto flush = [&] {
                if (run.nodes.size() >= 2) {
                    set.ribbons.push_back(std::move(run));
                }
                run = {};
            };
            for (const RiverPoint& pt : river.points) {
                // Only courses wide enough to read at kilometres
                // (the pinned tier); the sea sheet covers estuaries.
                if (pt.halfWidth < 4.0f ||
                    pt.surface <= seaLevel + 0.25f) {
                    flush();
                    continue;
                }
                if (!run.nodes.empty()) {
                    const auto& last = run.nodes.back();
                    const f32 dx = pt.x - last.x;
                    const f32 dz = pt.z - last.z;
                    if (dx * dx + dz * dz < 48.0f * 48.0f) {
                        continue; // ~48 m spacing is plenty far away
                    }
                }
                run.nodes.push_back(
                    { pt.x, pt.z, pt.surface, pt.halfWidth });
            }
            flush();
        }
    }
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
    const auto masters = render::terraingen::masterRiversNear(
        render::terraingen::ProceduralControls { controls }, macro, net,
        minX, minZ, maxX, maxZ);
    for (const render::terraingen::MasterRiver& river : masters) {
        render::WaterSystem::FarWaterSet::Ribbon run;
        for (const render::terraingen::MasterNode& node : river.nodes) {
            if (node.surface <= seaLevel + 0.5f ||
                inCovered(node.x, node.z)) {
                if (run.nodes.size() >= 2) {
                    set.ribbons.push_back(std::move(run));
                }
                run = {};
                continue;
            }
            const f32 hw = glm::clamp(
                0.008f * std::sqrt(glm::max(node.area, 0.0f)), 4.0f,
                60.0f);
            run.nodes.push_back({ node.x, node.z, node.surface, hw });
        }
        if (run.nodes.size() >= 2) {
            set.ribbons.push_back(std::move(run));
        }
    }
    return set;
}

} // namespace game
