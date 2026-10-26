#include "game/scenes/SandboxWorldController.hpp"

#include <cmath>
#include <iterator>
#include <string>

#include <glm/glm.hpp>

#include "engine/core/Clock.hpp"
#include "engine/core/Log.hpp"
#include "engine/platform/Paths.hpp"
#include "engine/render/WorldRenderer.hpp"
#include "engine/terrain/HeightPatches.hpp"
#include "engine/terrain/SandboxTerrain.hpp"
#include "engine/terrain/WaterBodies.hpp"
#include "engine/terrain/generation/WaterSolve.hpp"
#include "game/MapBaker.hpp"
#include "game/TerrainCollision.hpp"
#include "game/VegetationCollision.hpp"
#include "game/scenes/LandscapeTuning.hpp"
#include "world/terrain/TerrainRegions.hpp"
#include "world/terrain/WaterBodiesBuilder.hpp"

namespace game {

namespace {

struct Rect {
    f32 minX, minZ, maxX, maxZ;
};

// The bbox of a course's CENTRELINE (the ribbon's half-width excluded):
// the residency tests key on its centre. Caller guarantees points.
Rect riverBounds(const render::terraingen::River& river) {
    Rect r { 1.0e30f, 1.0e30f, -1.0e30f, -1.0e30f };
    for (const render::terraingen::RiverPoint& pt : river.points) {
        r.minX = glm::min(r.minX, pt.x);
        r.maxX = glm::max(r.maxX, pt.x);
        r.minZ = glm::min(r.minZ, pt.z);
        r.maxZ = glm::max(r.maxZ, pt.z);
    }
    return r;
}

// Chebyshev distance from the focus to the lake's BBOX, not its
// center: a lake spanning kilometres must stay resident while the
// player stands on an arm far from its middle.
f32 lakeBboxDistance(const render::terraingen::Lake& lake,
                     const Vec3& focus) {
    const f32 dx =
        glm::max(glm::max(lake.minX - focus.x, focus.x - lake.maxX), 0.0f);
    const f32 dz =
        glm::max(glm::max(lake.minZ - focus.z, focus.z - lake.maxZ), 0.0f);
    return glm::max(dx, dz);
}

// The 64 m chunk keys of `rect` clamped to the square of `reach` around
// the focus (empty intersection: none).
template <class Fn>
void forEachChunkWithinReach(const Rect& rect, const Vec3& focus, f32 reach,
                             Fn&& fn) {
    const f32 minX = glm::max(rect.minX, focus.x - reach);
    const f32 maxX = glm::min(rect.maxX, focus.x + reach);
    const f32 minZ = glm::max(rect.minZ, focus.z - reach);
    const f32 maxZ = glm::min(rect.maxZ, focus.z + reach);
    if (minX > maxX || minZ > maxZ) {
        return;
    }
    for (i32 cz = static_cast<i32>(std::floor(minZ / 64.0f));
         cz <= static_cast<i32>(std::floor(maxZ / 64.0f)); ++cz) {
        for (i32 cx = static_cast<i32>(std::floor(minX / 64.0f));
             cx <= static_cast<i32>(std::floor(maxX / 64.0f)); ++cx) {
            fn(render::HeightPatches::keyOf(cx, cz));
        }
    }
}

} // namespace

std::filesystem::path SandboxWorldController::cacheRoot(u32 terrainSeed) {
    return platform::executableDir() / "terrain-cache" /
           std::to_string(terrainSeed);
}

void SandboxWorldController::rebuildCollision(const SandboxContext& ctx,
                                              bool onlyIfBuilt) {
    if (onlyIfBuilt && !(ctx.physics && ctx.terrainCollision)) {
        return;
    }
    ctx.terrainCollision = std::make_unique<TerrainCollision>(
        *ctx.physics, ctx.terrainParams, &ctx.jobs);
    ctx.vegCollision = std::make_unique<VegetationCollision>(
        *ctx.physics, ctx.terrainParams, &ctx.jobs);
}

void SandboxWorldController::republishWorld(const SandboxContext& ctx) {
    render::TerrainParams& params = ctx.terrainParams;
    // A world-sized event keeps the contentTouchedSince contract (every
    // bump records its rects).
    params.contentEvents.push(params.contentStamp + 1, -1.0e9f, -1.0e9f,
                              1.0e9f, 1.0e9f);
    ++params.contentStamp;
    ctx.renderer.requestRegenerate();
    ctx.renderer.invalidateOcclusion();
    rebuildCollision(ctx, /*onlyIfBuilt=*/true);
}

u32 SandboxWorldController::loadNeighbourOverviews(
    const SandboxContext& ctx, render::SandboxTerrain& sb,
    const render::terraingen::TileBakeParams& bakeParams) const {
    // Cheap (a manifest read per missing neighbour): polled every 2 s so
    // a background prefetch shows up on the horizon without a relaunch.
    const auto root = cacheRoot(ctx.tuning.terrainSeed);
    u32 added = 0;
    for (i32 dz = -1; dz <= 1; ++dz) {
        for (i32 dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dz == 0) {
                continue;
            }
            const i32 mx = mapX_ + dx;
            const i32 mz = mapZ_ + dz;
            bool held = false;
            for (const auto& o : sb.neighbourOverviews) {
                held = held || (o.mapX == mx && o.mapZ == mz);
            }
            if (held || !mapBakedAndValid(root, mx, mz, kMapTilesPerSide,
                                          &bakeParams)) {
                continue;
            }
            if (auto overview = loadMapOverview(mapCacheDir(root, mx, mz))) {
                sb.neighbourOverviews.push_back(
                    { mx, mz, overview->grid, std::move(overview->heights) });
                ++added;
            }
        }
    }
    if (added > 0) {
        LOG_INFO("Sandbox terrain: {} neighbour overview(s) loaded ({} held)",
                 added, sb.neighbourOverviews.size());
    }
    return added;
}

Vec3 SandboxWorldController::probeSpawn(const SandboxContext& ctx) const {
    const render::TerrainParams& params = ctx.terrainParams;
    const auto& sandbox = params.sandbox;
    const f32 size = sandbox->grid.mapSize;
    // The map's cached water (nothing is published yet): the probe
    // must not pick a lake bed -- the flattest ground there is.
    const render::WaterSystem::FarWaterSet water = makeFarWaterProvider(
        ctx, mapX_, mapZ_, ctx.mapBakeParams(), sandbox->grid,
        ctx.renderer.waterSystem().simConfig().pinRiverHalfWidth)(
        (static_cast<f32>(mapX_) + 0.5f) * size,
        (static_cast<f32>(mapZ_) + 0.5f) * size, size * 0.5f);
    if (const auto spot = render::probeMapSpawn(
            *sandbox, mapX_, mapZ_, ctx.tuning.seaLevel,
            [&water](f32 x, f32 z) { return farWaterWetAt(water, x, z); })) {
        return *spot;
    }
    return { (static_cast<f32>(mapX_) + 0.5f) * size, 0.0f,
             (static_cast<f32>(mapZ_) + 0.5f) * size };
}

void SandboxWorldController::travelTo(const SandboxContext& ctx, i32 mapX,
                                      i32 mapZ, const Vec2* arrival) {
    // Procedural maps share ONE worldspace record (the implicit-cell
    // grid separates their content by coordinates).
    applyMap(ctx, mapX, mapZ);
    ctx.renderer.setStreamingHold(true);
    if (arrival) {
        // Land at the pass's far side, grounded on the map fallback
        // (overview when baked, shaped analytic otherwise); the warmup
        // spawn validation refines it on the real baked ground.
        const render::TerrainParams& params = ctx.terrainParams;
        const f32 h = render::terrain::height(params, arrival->x, arrival->y);
        spawn_ = { arrival->x, h, arrival->y };
    } else {
        spawn_ = probeSpawn(ctx);
    }
    spawnValid_ = true;
    // A crossing must READ as one: hold the veil at least ~1 s even
    // with both maps warm in cache (the instant swap was unreadable at
    // the col). The arrival is an AUTHORED point: the wet-spawn
    // validation may nudge it a couple hundred meters (a pond at the
    // pass), never re-scout kilometers away.
    ctx.armWarmup(spawn_, 1.0f, 240.0f);
    // MOVE the traveler: the old position lies outside the new map's
    // rect — nothing streams there, the fallback reads open sea, and a
    // player left behind stares at water forever. Same mechanism as
    // performTravel: respawn the capsule, park the fly camera at the
    // spawn.
    ctx.reseatPlayer(spawn_);
    // A crossing is walked through, not teleported to: keep the look
    // direction (the start pose faced -Z, which read as "turning
    // around" at every line). The flag also covers the warmup's
    // re-placement once the slices land.
    keepHeading_ = arrival != nullptr;
    ctx.placeStartCamera();
    ctx.snapCellEntities();
}

void SandboxWorldController::requestTravel(i32 mapX, i32 mapZ,
                                           bool hasArrival,
                                           const Vec2& arrival) {
    pendingTravel_ = PendingTravel { mapX, mapZ, hasArrival, arrival };
}

void SandboxWorldController::runPendingTravel(const SandboxContext& ctx) {
    if (!pendingTravel_) {
        return;
    }
    const PendingTravel travel = *pendingTravel_;
    pendingTravel_.reset();
    travelTo(ctx, travel.mapX, travel.mapZ,
             travel.hasArrival ? &travel.arrival : nullptr);
}

void SandboxWorldController::queueCrossing(const SandboxContext& ctx,
                                           const Vec3& at) {
    const auto& sandbox = ctx.terrainParams.sandbox;
    if (!(sandbox && sandbox->grid.valid)) {
        return;
    }
    const f32 size = sandbox->grid.mapSize;
    const f32 minX = static_cast<f32>(mapX_) * size;
    const f32 minZ = static_cast<f32>(mapZ_) * size;
    const f32 out = 8.0f; // hysteresis past the line
    const i32 dx = at.x < minX - out          ? -1
                   : at.x > minX + size + out ? 1
                                              : 0;
    const i32 dz = at.z < minZ - out          ? -1
                   : at.z > minZ + size + out ? 1
                                              : 0;
    if (dx != 0 || dz != 0) {
        pendingTravel_ = PendingTravel { mapX_ + dx, mapZ_ + dz, true,
                                         Vec2 { at.x, at.z } };
    }
}

void SandboxWorldController::updatePrefetch(const SandboxContext& ctx, f32 dt,
                                            const Vec3& cameraPosition) {
    // Nearing a rim warms the neighbour map in the background — the
    // crossing then costs a fade, not a bake. Cheap check every 2 s.
    prefetchCooldown_ -= dt;
    if (!(ctx.sandboxActive && streamer_ && prefetchCooldown_ <= 0.0f)) {
        return;
    }
    prefetchCooldown_ = 2.0f;
    const auto& sandbox = ctx.terrainParams.sandbox;
    if (!(sandbox && sandbox->grid.valid)) {
        return;
    }
    const Vec3 at = cameraPosition;
    const f32 nearRim = 1536.0f;
    const f32 size = sandbox->grid.mapSize;
    const f32 minX = static_cast<f32>(mapX_) * size;
    const f32 minZ = static_cast<f32>(mapZ_) * size;
    const i32 nx = at.x - minX < nearRim          ? -1
                   : minX + size - at.x < nearRim ? 1
                                                  : 0;
    const i32 nz = at.z - minZ < nearRim          ? -1
                   : minZ + size - at.z < nearRim ? 1
                                                  : 0;
    if (nx != 0) {
        streamer_->prefetchMap(mapX_ + nx, mapZ_);
    }
    if (nz != 0) {
        streamer_->prefetchMap(mapX_, mapZ_ + nz);
    }
    if (nx != 0 && nz != 0) {
        streamer_->prefetchMap(mapX_ + nx, mapZ_ + nz);
    }
    // Neighbour overviews landing (a prefetch finished): publish a new
    // sandbox identity holding them — the horizon past the rim becomes
    // the neighbour's own truth — and bump the content stamp on that
    // rect.
    auto next = std::make_shared<render::SandboxTerrain>(*sandbox);
    if (loadNeighbourOverviews(ctx, *next, ctx.mapBakeParams()) > 0) {
        render::TerrainParams& tp = ctx.terrainParams;
        tp.contentEvents.push(tp.contentStamp + 1, minX - size, minZ - size,
                              minX + 2.0f * size, minZ + 2.0f * size);
        ++tp.contentStamp;
        tp.sandbox = next;
    }
}

render::WaterSystem::FarWaterFn SandboxWorldController::makeFarWaterProvider(
    const SandboxContext& ctx, i32 mapX, i32 mapZ,
    const render::terraingen::TileBakeParams& bake,
    const render::terraingen::MapGridSpec& grid, f32 pinHalfWidth) const {
    render::terraingen::ProceduralControlParams cp = bake.controls;
    cp.seed = bake.worldSeed;
    render::terraingen::MasterNetworkParams net = bake.network;
    net.seaLevel = bake.macro.seaLevel;
    net.mapGrid = grid;
    return [cp, macro = bake.macro, net, grid, law = bake.hydrology.widthLaw(),
            pinHalfWidth, sea = ctx.tuning.seaLevel, tileSize = bake.tileSize,
            root = cacheRoot(ctx.tuning.terrainSeed), mapX,
            mapZ](f32 cx, f32 cz, f32 halfSpan) {
        return collectFarWater(root, mapX, mapZ, tileSize, cp, macro, net,
                               grid, law, pinHalfWidth, sea, cx, cz,
                               halfSpan);
    };
}

void SandboxWorldController::applyMap(const SandboxContext& ctx, i32 mapX,
                                      i32 mapZ) {
    // A crossing (a sandbox map was already streaming) keeps the
    // previous map's resident slices and water -- see the base below.
    const bool crossing = ctx.sandboxActive && streamer_ != nullptr;
    mapX_ = mapX;
    mapZ_ = mapZ;
    render::TerrainParams& params = ctx.terrainParams;
    auto sandbox = std::make_shared<render::SandboxTerrain>();
    // The sandbox world IS a bounded map; the fallback beyond the
    // borders shows the lattice neighbours' preview. The fallback and
    // the spawn probe read the SAME params as the bake (the pupitre
    // included).
    const render::terraingen::TileBakeParams bakeParams = ctx.mapBakeParams();
    sandbox->controls = bakeParams.controls;
    sandbox->controls.seed = ctx.tuning.terrainSeed;
    sandbox->macro = bakeParams.macro;
    TerrainBakeStreamer::MapStreamConfig mapCfg;
    mapCfg.tilesPerSide = kMapTilesPerSide;
    mapCfg.mapX = mapX;
    mapCfg.mapZ = mapZ;
    sandbox->grid.valid = true;
    sandbox->grid.seed = ctx.tuning.terrainSeed;
    sandbox->grid.mapSize =
        bakeParams.tileSize * static_cast<f32>(mapCfg.tilesPerSide);
    sandbox->grid.seaLevel = ctx.tuning.seaLevel;
    // The baked map's overview (when the cache holds it): the
    // fallback inside the map becomes the map's own coarse truth.
    // A cold cache boots on the analytic + rim shape and picks the
    // overview up on the NEXT session (the background bake writes
    // it; hot-swapping it mid-session would move the fallback
    // ground under unbaked chunks).
    const auto root = cacheRoot(ctx.tuning.terrainSeed);
    if (const auto overview =
            loadMapOverview(mapCacheDir(root, mapCfg.mapX, mapCfg.mapZ))) {
        sandbox->overviewGrid = overview->grid;
        sandbox->overview = std::move(overview->heights);
        LOG_INFO("Sandbox terrain: map overview loaded ({}x{} at "
                 "{:.0f} m)",
                 overview->grid.n, overview->grid.n,
                 overview->grid.texelSize);
    }
    loadNeighbourOverviews(ctx, *sandbox, bakeParams);
    params.sandbox = sandbox;
    ctx.activeSnowLine = ctx.tuning.sandboxSnowLine;
    streamer_ = std::make_unique<TerrainBakeStreamer>(bakeParams, root,
                                                      &ctx.jobs, mapCfg);
    // The lake registry is per map: the new streamer re-scans.
    mapLakes_.clear();
    mapLakesValid_ = false;
    lakeRefreshFocusValid_ = false;
    LOG_INFO("Sandbox terrain: seed {}, map ({}, {}) = {:.0f} m "
             "island, {} m slices",
             ctx.tuning.terrainSeed, mapX, mapZ, sandbox->grid.mapSize,
             bakeParams.tileSize);
    // Live water sim (option C): boundary inflow from the master
    // network — worker-callable and pure (the memoized network
    // makes repeat queries cheap after the first).
    {
        render::terraingen::ProceduralControlParams cp = bakeParams.controls;
        cp.seed = bakeParams.worldSeed;
        render::terraingen::MasterNetworkParams net = bakeParams.network;
        net.seaLevel = bakeParams.macro.seaLevel;
        net.mapGrid = sandbox->grid;
        render::terraingen::WaterSolveParams solveParams =
            bakeParams.waterSolve;
        solveParams.seaLevel = bakeParams.macro.seaLevel;
        const render::terraingen::MacroParams macro = bakeParams.macro;
        ctx.renderer.waterSystem().setSimSources(
            [cp, macro, net, solveParams](f32 minX, f32 minZ, f32 maxX,
                                          f32 maxZ) {
                return render::terraingen::masterBoundarySources(
                    render::terraingen::ProceduralControls { cp }, macro,
                    net, solveParams, minX, minZ, maxX, maxZ);
            });
        ctx.renderer.waterSystem().simConfig().params.seaLevel =
            ctx.tuning.seaLevel;
        // Far water: distant lakes/rivers for the FarTerrain — cached
        // .twb + master fleuves, gathered on the worker.
        ctx.renderer.waterSystem().setFarWater(makeFarWaterProvider(
            ctx, mapX, mapZ, bakeParams, sandbox->grid,
            ctx.renderer.waterSystem().simConfig().pinRiverHalfWidth));
    }
    // The base: the authored regions, plus -- on a crossing -- the
    // previous map's resident slices and water outside the new rect.
    // The ground behind the traveler stays the SAME ground (heights,
    // masks, hence materials and scatter), not the neighbour's 64 m
    // overview; the distance eviction in publishTiles prunes them like
    // the active map's own tiles. Slices inside the new rect are
    // dropped: the fresh streamer republishes them from the cache (its
    // published set starts empty).
    {
        const f32 size = sandbox->grid.mapSize;
        const f32 minX = static_cast<f32>(mapX) * size;
        const f32 minZ = static_cast<f32>(mapZ) * size;
        const auto inNewRect = [&](f32 cx, f32 cz) {
            return cx >= minX && cx < minX + size && cz >= minZ &&
                   cz < minZ + size;
        };
        const sptr<const render::TerrainBase> authored =
            world::buildTerrainBase(ctx.forms, ctx.assetDb);
        auto base = std::make_shared<render::TerrainBase>();
        base->regions = authored->regions;
        if (crossing && ctx.terrainBase) {
            const auto isAuthored = [&](const render::TerrainRegion& r) {
                for (const auto& a : authored->regions) {
                    if (a->originX == r.originX && a->originZ == r.originZ &&
                        a->width == r.width && a->height == r.height) {
                        return true;
                    }
                }
                return false;
            };
            u32 kept = 0;
            for (const auto& rp : ctx.terrainBase->regions) {
                const render::TerrainRegion& r = *rp;
                if (isAuthored(r) || inNewRect(r.originX + r.spanX() * 0.5f,
                                               r.originZ + r.spanZ() * 0.5f)) {
                    continue;
                }
                base->regions.push_back(rp);
                ++kept;
            }
            std::erase_if(lakes_, [&](const render::terraingen::Lake& lake) {
                return inNewRect((lake.minX + lake.maxX) * 0.5f,
                                 (lake.minZ + lake.maxZ) * 0.5f);
            });
            std::erase_if(rivers_,
                          [&](const render::terraingen::River& river) {
                              if (river.points.empty()) {
                                  return true;
                              }
                              const Rect b = riverBounds(river);
                              return inNewRect((b.minX + b.maxX) * 0.5f,
                                               (b.minZ + b.maxZ) * 0.5f);
                          });
            LOG_INFO("Sandbox terrain: {} slices of the previous map "
                     "stay resident across the line",
                     kept);
        } else {
            lakes_.clear();
            rivers_.clear();
        }
        base->buildIndex();
        ctx.terrainBase = base;
        params.base = ctx.terrainBase;
    }
    publishWaterBodies(ctx);
    params.snowLine = ctx.activeSnowLine;
    ctx.resetWorldTransients();
    republishWorld(ctx);
}

void SandboxWorldController::restoreAuthoredWorld(const SandboxContext& ctx) {
    render::TerrainParams& params = ctx.terrainParams;
    spawnValid_ = false;
    params.sandbox = nullptr;
    ctx.activeSnowLine = ctx.tuning.snowLine;
    streamer_.reset();
    ctx.renderer.setStreamingHold(false);
    lakes_.clear();
    rivers_.clear();
    mapLakes_.clear();
    mapLakesValid_ = false;
    lakeRefreshFocusValid_ = false;
    // Back to the authored layers only.
    ctx.terrainBase = world::buildTerrainBase(ctx.forms, ctx.assetDb);
    params.base = ctx.terrainBase;
    publishWaterBodies(ctx);
    // The sim window runs here too (authored lakes pinned, rain,
    // the player's springs) — without the sandbox's master-network
    // inflows, which would answer for another world.
    ctx.renderer.waterSystem().setSimSources({});
    params.snowLine = ctx.activeSnowLine;
    republishWorld(ctx);
}

void SandboxWorldController::publishWaterBodies(const SandboxContext& ctx) {
    auto next = std::make_shared<render::WaterBodies>(
        *world::buildWaterBodies(ctx.forms, ctx.tuning.seaLevel));
    // Baked lakes + river ribbons are the OUTSIDE-WINDOW water (and
    // the fallback while the sim spins up): the real-time windowed
    // sim (option C, docs/PAYSAGE.md §2.3) owns everything near the
    // camera and replaces these per fragment inside its trusted rect.
    for (const render::terraingen::Lake& lake : lakes_) {
        render::LakeSurface surface;
        surface.level = lake.level;
        surface.minX = lake.minX;
        surface.minZ = lake.minZ;
        surface.maxX = lake.maxX;
        surface.maxZ = lake.maxZ;
        surface.maskWidth = lake.maskWidth;
        surface.maskHeight = lake.maskHeight;
        surface.maskTexel = lake.maskTexel;
        surface.mask = lake.mask;
        next->lakes.push_back(std::move(surface));
    }
    for (const render::terraingen::River& river : rivers_) {
        render::RiverSurface surface;
        surface.headType = river.headType;
        surface.headIsSource = river.headIsSource;
        surface.flowSpeed =
            render::terraingen::riverFlowSpeedForTier(river.tier);
        surface.minX = surface.minZ = 1.0e30f;
        surface.maxX = surface.maxZ = -1.0e30f;
        for (const render::terraingen::RiverPoint& pt : river.points) {
            render::RiverNode node;
            node.x = pt.x;
            node.z = pt.z;
            node.surface = pt.surface;
            node.halfWidth = pt.halfWidth;
            node.area = pt.area;
            surface.nodes.push_back(node);
            surface.minX = glm::min(surface.minX, pt.x - pt.halfWidth);
            surface.maxX = glm::max(surface.maxX, pt.x + pt.halfWidth);
            surface.minZ = glm::min(surface.minZ, pt.z - pt.halfWidth);
            surface.maxZ = glm::max(surface.maxZ, pt.z + pt.halfWidth);
        }
        if (surface.nodes.size() >= 2) {
            next->rivers.push_back(std::move(surface));
        }
    }
    ctx.waterBodies = next;
    ctx.renderer.waterSystem().setBodies(ctx.waterBodies);
    // The scatter rules read the same set (underLocalWater): no trees
    // or grass under altitude lakes/rivers. Same immutable-publish
    // contract as base/patches.
    ctx.terrainParams.water = ctx.waterBodies;
    if (!next->lakes.empty() || !next->rivers.empty()) {
        LOG_INFO("Water: {} lake(s), {} river(s)", next->lakes.size(),
                 next->rivers.size());
    }
}

bool SandboxWorldController::refreshLakes(const Vec3& focus) {
    if (!mapLakesValid_ || !streamer_) {
        return false;
    }
    const f32 evict = streamer_->tileSize() * 2.5f;
    // Identity = the exact header fields both copies read from the
    // same .twb bytes (one hydrology per map: one level per basin).
    const auto sameLake = [](const render::terraingen::Lake& a,
                             const render::terraingen::Lake& b) {
        return a.level == b.level && a.minX == b.minX && a.minZ == b.minZ &&
               a.maxX == b.maxX && a.maxZ == b.maxZ;
    };
    vector<render::terraingen::Lake> next;
    // Residents of a PREVIOUS map kept across a crossing are not in
    // this map's registry: they stay, under the same bbox eviction.
    for (const render::terraingen::Lake& lake : lakes_) {
        bool governed = false;
        for (const render::terraingen::Lake& reg : mapLakes_) {
            if (sameLake(lake, reg)) {
                governed = true;
                break;
            }
        }
        if (!governed && lakeBboxDistance(lake, focus) <= evict) {
            next.push_back(lake);
        }
    }
    for (const render::terraingen::Lake& lake : mapLakes_) {
        if (lakeBboxDistance(lake, focus) <= evict) {
            next.push_back(lake);
        }
    }
    bool changed = next.size() != lakes_.size();
    if (!changed) {
        for (size_t i = 0; i < next.size(); ++i) {
            if (!sameLake(next[i], lakes_[i])) {
                changed = true;
                break;
            }
        }
    }
    if (changed) {
        lakes_ = std::move(next);
    }
    lastLakeRefreshFocus_ = focus;
    lakeRefreshFocusValid_ = true;
    return changed;
}

void SandboxWorldController::update(const SandboxContext& ctx,
                                    const Vec3& focus) {
    if (!streamer_) {
        return;
    }
    // Drain the WHOLE mailbox, publish once: N tiles used to mean N
    // water rebuilds, N collision rebuilds and N queue passes.
    vector<TerrainBakeStreamer::PublishedTile> batch;
    streamer_->update(focus,
                      [&batch](TerrainBakeStreamer::PublishedTile&& tile) {
                          batch.push_back(std::move(tile));
                      });
    // Map-wide lake registry: lands once per map, then membership
    // follows the focus (bbox distance) even between tile events —
    // a lake must be pinnable wherever the player stands on it,
    // not only while its owner slice is published.
    vector<render::terraingen::Lake> registry;
    bool lakesChanged = false;
    if (streamer_->takeMapLakes(registry)) {
        mapLakes_ = std::move(registry);
        mapLakesValid_ = true;
        LOG_INFO("Sandbox water: map lake registry loaded "
                 "({} lakes)",
                 mapLakes_.size());
        lakesChanged = refreshLakes(focus);
    } else if (mapLakesValid_ &&
               (!lakeRefreshFocusValid_ ||
                glm::max(std::abs(focus.x - lastLakeRefreshFocus_.x),
                         std::abs(focus.z - lastLakeRefreshFocus_.z)) >
                    512.0f)) {
        lakesChanged = refreshLakes(focus);
    }
    if (!batch.empty()) {
        publishTiles(ctx, std::move(batch), focus);
    } else if (lakesChanged) {
        publishWaterBodies(ctx);
        // A lakes-only arrival (registry landing, membership
        // refresh) changes the WATER under chunks that were
        // scattered before it: without a re-scatter, trees and
        // grass placed on a then-dry bed stand in the new sheet
        // until the chunk re-streams. Ground is untouched, so
        // only the scatter queue runs — bounded to resident
        // chunks that a lake actually covers.
        const f32 reach =
            static_cast<f32>(ctx.tuning.terrainViewRadius + 1) * 64.0f;
        vector<u64> scatterKeys;
        for (const auto& lake : lakes_) {
            forEachChunkWithinReach(
                Rect { lake.minX, lake.minZ, lake.maxX, lake.maxZ }, focus,
                reach, [&](u64 key) { scatterKeys.push_back(key); });
        }
        ctx.renderer.requestRescatter(scatterKeys);
    }
}

void SandboxWorldController::publishTiles(
    const SandboxContext& ctx,
    vector<TerrainBakeStreamer::PublishedTile>&& tiles, const Vec3& focus) {
    // Section marks for the publish-spike breakdown logged at the end —
    // the numbers that order the incremental-invalidation work
    // (docs/CPU-PERF.md).
    const core::TimePoint publishStart = core::clockNow();
    core::TimePoint mark = publishStart;
    const auto sectionMs = [&mark] {
        const f64 ms = core::millisecondsSince(mark);
        mark = core::clockNow();
        return ms;
    };
    auto next = std::make_shared<render::TerrainBase>();
    if (ctx.terrainBase) {
        next->regions = ctx.terrainBase->regions;
    }
    // Changed rects (published AND evicted) feed the content-event ring
    // and the intersection gates below.
    vector<Rect> changedRects;
    // Bounded residency (sandbox streaming only): drop far-behind tiles;
    // the streamer re-requests them on the way back. Editor previews
    // (no streamer) keep every published region.
    if (streamer_) {
        const f32 tileSize = streamer_->tileSize();
        const f32 evict = tileSize * 2.5f;
        std::erase_if(next->regions,
                      [&](const sptr<const render::TerrainRegion>& rp) {
                          const render::TerrainRegion& r = *rp;
                          const f32 cx = r.originX + r.spanX() * 0.5f;
                          const f32 cz = r.originZ + r.spanZ() * 0.5f;
                          const bool out = std::abs(cx - focus.x) > evict ||
                                           std::abs(cz - focus.z) > evict;
                          if (out) {
                              streamer_->forgetTile(
                                  static_cast<i32>(std::floor(cx / tileSize)),
                                  static_cast<i32>(std::floor(cz / tileSize)));
                              changedRects.push_back(
                                  { r.originX, r.originZ,
                                    r.originX + r.spanX(),
                                    r.originZ + r.spanZ() });
                          }
                          return out;
                      });
        // The water follows the regions out: the flat lake/river
        // arrays used to grow forever, and every swim query and
        // reconcile scanned them all. Lakes: registry-driven selection
        // once the map scan landed (bbox distance — a big lake stays
        // while the player is anywhere ON it); the bbox fallback below
        // covers the frames before the scan.
        if (mapLakesValid_) {
            refreshLakes(focus);
        } else {
            std::erase_if(lakes_, [&](const render::terraingen::Lake& lake) {
                return lakeBboxDistance(lake, focus) > evict;
            });
        }
        std::erase_if(rivers_, [&](const render::terraingen::River& river) {
            if (river.points.empty()) {
                return true;
            }
            const Rect b = riverBounds(river);
            return std::abs((b.minX + b.maxX) * 0.5f - focus.x) > evict ||
                   std::abs((b.minZ + b.maxZ) * 0.5f - focus.z) > evict;
        });
    }
    const f64 evictMs = sectionMs();
    const size_t firstNew = next->regions.size();
    for (TerrainBakeStreamer::PublishedTile& tile : tiles) {
        next->regions.emplace_back(std::make_shared<render::TerrainRegion>(
            std::move(tile.region)));
        if (!mapLakesValid_) {
            // Pre-registry fallback only: once the map scan landed,
            // refreshLakes owns lake membership (a tile's lakes are a
            // subset of the registry — inserting them here would
            // duplicate the sheets).
            lakes_.insert(lakes_.end(), tile.lakes.begin(), tile.lakes.end());
        }
        rivers_.insert(rivers_.end(),
                       std::make_move_iterator(tile.rivers.begin()),
                       std::make_move_iterator(tile.rivers.end()));
    }
    for (size_t r = firstNew; r < next->regions.size(); ++r) {
        const render::TerrainRegion& region = *next->regions[r];
        changedRects.push_back({ region.originX, region.originZ,
                                 region.originX + region.spanX(),
                                 region.originZ + region.spanZ() });
    }
    next->buildIndex(); // regions final: per-sample scans go O(bucket)
    ctx.terrainBase = next;
    ctx.terrainParams.base = next;
    // The stamp bump + its scoped events (contentTouchedSince contract:
    // every bump records its rects).
    {
        render::TerrainParams& params = ctx.terrainParams;
        const u64 stamp = params.contentStamp + 1;
        for (const Rect& rect : changedRects) {
            params.contentEvents.push(stamp, rect.minX, rect.minZ, rect.maxX,
                                      rect.maxZ);
        }
        ++params.contentStamp; // FarTerrain/far-water rebake (unscoped)
    }
    ctx.renderer.setStreamingHold(false); // base regions exist: rings may stream
    // Remesh AND re-scatter the resident chunks the new regions cover
    // within the view ring (the deferred queues skip chunks that are
    // not resident). Both queues, like the sculpt commit:
    // grass/vegetation baked before the tile landed sit on the OLD
    // heights — without the scatter pass they float over the new ground.
    const f32 reach =
        static_cast<f32>(ctx.tuning.terrainViewRadius + 1) * 64.0f;
    vector<u64> rebuildKeys;
    for (size_t r = firstNew; r < next->regions.size(); ++r) {
        const render::TerrainRegion& region = *next->regions[r];
        forEachChunkWithinReach(
            Rect { region.originX, region.originZ,
                   region.originX + region.spanX(),
                   region.originZ + region.spanZ() },
            focus, reach, [&](u64 key) { rebuildKeys.push_back(key); });
    }
    ctx.renderer.requestRemesh(rebuildKeys);
    ctx.renderer.requestRescatter(rebuildKeys);
    // Horizon table: only when a changed region reaches the occlusion
    // rays (camera square of the view reach).
    const auto anyRectIntersects = [&](f32 minX, f32 minZ, f32 maxX,
                                       f32 maxZ) {
        for (const Rect& rect : changedRects) {
            if (rect.maxX >= minX && rect.minX <= maxX && rect.maxZ >= minZ &&
                rect.minZ <= maxZ) {
                return true;
            }
        }
        return false;
    };
    if (anyRectIntersects(focus.x - reach, focus.z - reach, focus.x + reach,
                          focus.z + reach)) {
        ctx.renderer.invalidateOcclusion();
    }
    const f64 regionsMs = sectionMs();
    rebuildCollision(ctx, /*onlyIfBuilt=*/true);
    ctx.snapCellEntities();
    const f64 collisionMs = sectionMs();
    // (The cross-tile duplicate-lake suppression lived here: one
    // hydrology per MAP gives every basin a single identity — there is
    // nothing left to dedupe.)
    const f64 dedupeMs = sectionMs();
    // (The runtime lake re-validation lived here: the in-bake
    // reconcile now sees past a slice's rect through the map fallback,
    // and slices of one map are carve-coherent — the belt is gone with
    // the divergence it guarded.)
    const f64 reconcileMs = sectionMs();
    publishWaterBodies(ctx);
    // Fresh regions changed the ground under the sim window — it
    // re-samples on its next step job. Scoped: a publish that never
    // reaches the 512 m window keeps the sim (and its breadcrumb cache)
    // untouched.
    bool simTouched = false;
    for (const Rect& rect : changedRects) {
        if (ctx.renderer.waterSystem().simWindowIntersects(
                rect.minX, rect.minZ, rect.maxX, rect.maxZ)) {
            simTouched = true;
            break;
        }
    }
    if (simTouched) {
        ctx.renderer.waterSystem().notifySimGroundChanged();
    }
    const f64 republishMs = sectionMs();
    LOG_INFO("Sandbox terrain: {} tile(s) published ({} lakes, {} "
             "rivers resident)",
             tiles.size(), lakes_.size(), rivers_.size());
    LOG_INFO("publish breakdown: {:.1f} ms total — evict {:.1f} | "
             "regions {:.1f} | collision {:.1f} | dedupe {:.1f} | "
             "reconcile {:.1f} | republish {:.1f}",
             core::millisecondsSince(publishStart), evictMs, regionsMs,
             collisionMs, dedupeMs, reconcileMs, republishMs);
}

void SandboxWorldController::finalizeSpawn(const SandboxContext& ctx,
                                           f32 spawnSearchRadius) {
    // Called ONCE by the warmup, after the whole spawn ring (terrain AND
    // water) is published: no per-publish re-checks, no ordering races
    // with neighbour-owned lakes.
    if (!ctx.sandboxActive || !spawnValid_) {
        return;
    }
    const render::TerrainParams& params = ctx.terrainParams;
    // The search must stay INSIDE the active map's rect: a crossing
    // arrival sits meters from the line, the previous map's kept
    // slices make the far side look dry and trustworthy, and a spawn
    // relocated across the line is immediately bounced back by the
    // crossing detector (travel ping-pong, measured on a west line).
    // Inset past the detector's 8 m hysteresis.
    const auto& sandboxParams = params.sandbox;
    const bool bounded = sandboxParams && sandboxParams->grid.valid;
    const f32 mapSize = bounded ? sandboxParams->grid.mapSize : 0.0f;
    const f32 rectMinX = static_cast<f32>(mapX_) * mapSize;
    const f32 rectMinZ = static_cast<f32>(mapZ_) * mapSize;
    constexpr f32 kRectInset = 16.0f;
    const auto inActiveMap = [&](f32 x, f32 z) {
        return !bounded || (x >= rectMinX + kRectInset &&
                            x <= rectMinX + mapSize - kRectInset &&
                            z >= rectMinZ + kRectInset &&
                            z <= rectMinZ + mapSize - kRectInset);
    };
    const auto wetAt = [&](f32 x, f32 z, f32& outH) {
        outH = render::terrain::height(params, x, z);
        if (!ctx.terrainBase || !ctx.terrainBase->regionAt(x, z)) {
            return true; // unbaked ground is not a trustworthy spot
        }
        if (outH < params.seaLevel + 2.0f) {
            return true;
        }
        return ctx.waterBodies &&
               render::terrain::waterSurfaceAt(*ctx.waterBodies, x, z,
                                               outH + 1.0f)
                   .has_value();
    };
    f32 ground = 0.0f;
    if (wetAt(spawn_.x, spawn_.z, ground)) {
        // Reach past the largest flood basins (they span kilometers in
        // the compressed world) — a failed search keeps the old spot
        // and logs, never silently.
        bool relocated = false;
        for (f32 radius = 60.0f; radius <= spawnSearchRadius && !relocated;
             radius += 60.0f) {
            for (u32 k = 0; k < 12 && !relocated; ++k) {
                const f32 angle = static_cast<f32>(k) * (6.2831853f / 12.0f);
                const f32 x = spawn_.x + std::cos(angle) * radius;
                const f32 z = spawn_.z + std::sin(angle) * radius;
                if (inActiveMap(x, z) && !wetAt(x, z, ground)) {
                    spawn_ = { x, ground, z };
                    relocated = true;
                }
            }
        }
        if (relocated) {
            LOG_INFO("Sandbox spawn was wet on the baked terrain: "
                     "moved to ({:.0f}, {:.0f})",
                     spawn_.x, spawn_.z);
        } else {
            LOG_WARN("Sandbox spawn: no dry baked ground within 6 km — "
                     "keeping the probed spot");
        }
    } else {
        // Dry — re-seat on the exact baked ground level (the analytic
        // probe guessed it).
        spawn_.y = ground;
    }
    ctx.placeStartCamera();
}

} // namespace game
