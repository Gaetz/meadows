#pragma once

#include <filesystem>
#include <functional>
#include <optional>

#include "engine/core/Defines.hpp"
#include "game/TerrainBakeStreamer.hpp" // the streamer, PublishedTile, Lake, River

namespace assets {
class AssetDatabase;
}
namespace core {
class JobSystem;
}
namespace data {
class FormDatabase;
struct LandscapeTuningForm;
}
namespace phys {
class PhysicsWorld;
}
namespace render {
class WorldRenderer;
struct TerrainBase;
struct TerrainParams;
struct WaterBodies;
struct SandboxTerrain;
}

namespace game {

class TerrainCollision;
class VegetationCollision;

// The scene systems the bounded-map world touches, bundled so the map
// swap, the tile publish and the travel bookkeeping are decoupled from
// LandscapeScene. The scene rebuilds it each call from its own members
// — references, a few scalars, and the actions that stay scene
// territory as closures (the InteractionContext pattern). Mirrors
// StreamingContext / WarmupContext.
//
// Shared publish layers stay SCENE members (the render view, the water
// queries, the spawn validation all read them) and are reached here by
// reference: every write keeps the immutable-publish contract (a new
// instance swapped in, never a mutation).
struct SandboxContext {
    render::WorldRenderer& renderer;      // water system, rebuild requests, holds
    render::TerrainParams& terrainParams; // the scene's world params (this controller writes them)
    const data::FormDatabase& forms;      // authored regions / water bodies
    const assets::AssetDatabase& assetDb;
    const data::LandscapeTuningForm& tuning; // seed, sea level, snow lines, view radius
    core::JobSystem& jobs;                // streamer bakes, collision cooks
    phys::PhysicsWorld* physics;          // null before setupGameplay
    uptr<TerrainCollision>& terrainCollision;  // rebuilt whenever the ground changes
    uptr<VegetationCollision>& vegCollision;
    sptr<const render::TerrainBase>& terrainBase;   // baked base layer (immutable-publish)
    sptr<const render::WaterBodies>& waterBodies;   // sea + lakes + rivers (immutable-publish)
    f32& activeSnowLine;  // snow altitude of the active world (feeds params + view)
    bool sandboxActive;   // the exterior identity (the scene's fact)
    // The resolved map-scale bake params (tuning records -> TileBakeParams),
    // built at the call so the fallback, the streamer and the spawn probe
    // read the SAME params as the bake.
    std::function<render::terraingen::TileBakeParams()> mapBakeParams;
    // The map changed under the world: gestures, fx, interior escapes and
    // the springs of the previous map go; this map's springs enter the sim.
    std::function<void()> resetWorldTransients;
    // Camera at the spawn (sandbox) or the story viewpoint — the scene's.
    std::function<void()> placeStartCamera;
    // Cell references re-seated on the new ground (StreamingController).
    std::function<void()> snapCellEntities;
    // A traveler's capsule and party moved onto `spawn` (no-op without a
    // live capsule).
    std::function<void(const Vec3& spawn)> reseatPlayer;
    // The loading veil armed on `target` with the spawn validation step.
    std::function<void(const Vec3& target, f32 minSeconds,
                       f32 spawnSearchRadius)>
        armWarmup;
};

// Owns the bounded-map world (docs/PAYSAGE.md §1.1): the active map
// coordinates, the tile streamer, the resident lakes/rivers and the
// map-wide lake registry, the probed spawn, the queued crossing and the
// neighbour prefetch. One transaction per world change:
//   applyMap           the map swap (identity, streamer, water closures,
//                      transient clears, collision, content invalidation)
//   restoreAuthoredWorld  the story half of the same swap
//   publishTiles       the frame's finished slices landed at once
//   travelTo           applyMap + a fresh spawn + the veil + the traveler moved
// The scene keeps setSandboxMode (the exterior identity flip + the
// warmup arm) and placeStartCamera, and reads mapX/mapZ, spawn and the
// streamer through the accessors.
class SandboxWorldController {
public:
    // The seed's cache root: <exe>/terrain-cache/<seed>; map dirs under
    // it come from game::mapCacheDir.
    static std::filesystem::path cacheRoot(u32 terrainSeed);

    // The map swap. A crossing (a map was already streaming) keeps the
    // previous map's resident slices and water outside the new rect.
    // Camera placement and cell snapping stay with the callers.
    void applyMap(const SandboxContext& ctx, i32 mapX, i32 mapZ);
    // Back to the authored layers only (the story world).
    void restoreAuthoredWorld(const SandboxContext& ctx);

    // Per frame while the policy streams bakes: converge/bake the slices
    // around `focus`, land the finished ones as ONE publish, follow the
    // lake registry. Main thread — the streamer's worker->main handoff
    // drains here.
    void update(const SandboxContext& ctx, const Vec3& focus);
    // Lands finished tiles as ONE transaction: a single TerrainBase
    // publish, one water rebuild, one collision rebuild and one queue
    // pass however many tiles arrived.
    void publishTiles(const SandboxContext& ctx,
                      vector<TerrainBakeStreamer::PublishedTile>&& tiles,
                      const Vec3& focus);
    // Rebuilds waterBodies from Forms + the resident bakes and hands it to
    // the water system (the swim queries read the shared pointer).
    void publishWaterBodies(const SandboxContext& ctx);

    // Map-to-map travel: the whole-world swap plus a fresh spawn on the
    // target map behind the veil. `arrival` (nullable) lands the traveler
    // THERE instead of the map's probed spawn — the far side of a pass.
    void travelTo(const SandboxContext& ctx, i32 mapX, i32 mapZ,
                  const Vec2* arrival = nullptr);
    // A crossing asked mid-frame (pass trigger Lua, console): queued, run
    // at a safe point by runPendingTravel, never inside an ECS iteration.
    void requestTravel(i32 mapX, i32 mapZ, bool hasArrival,
                       const Vec2& arrival);
    bool travelPending() const { return pendingTravel_.has_value(); }
    void runPendingTravel(const SandboxContext& ctx);
    // The focus left the active rect (8 m hysteresis past the line):
    // queue the crossing into the neighbour map.
    void queueCrossing(const SandboxContext& ctx, const Vec3& at);
    // Approach prefetch: nearing a rim warms the neighbour map in the
    // background; landed neighbour overviews join the fallback. Polled
    // every 2 s.
    void updatePrefetch(const SandboxContext& ctx, f32 dt,
                        const Vec3& cameraPosition);

    // A pleasant start on the ACTIVE map (render::probeMapSpawn over the
    // cached water); the map centre when nothing qualifies.
    Vec3 probeSpawn(const SandboxContext& ctx) const;
    void setSpawn(const Vec3& spawn) {
        spawn_ = spawn;
        spawnValid_ = true;
    }
    // The single-shot spawn validation on the FINAL baked+water world:
    // relocates a wet spawn within `spawnSearchRadius` (inside the map
    // rect), re-seats a dry one on the baked ground, places the camera.
    void finalizeSpawn(const SandboxContext& ctx, f32 spawnSearchRadius);

    // "The whole world changed": one world-sized content event, a
    // regenerate, the occlusion dropped and the collision rebuilt.
    void republishWorld(const SandboxContext& ctx);
    // Fresh terrain + vegetation collision tiles over the current params.
    // `onlyIfBuilt`: skip while the physics layer is not up yet (the data
    // bootstrap runs the first map swap before setupGameplay).
    static void rebuildCollision(const SandboxContext& ctx, bool onlyIfBuilt);

    TerrainBakeStreamer* streamer() const { return streamer_.get(); }
    i32 mapX() const { return mapX_; }
    i32 mapZ() const { return mapZ_; }
    const Vec3& spawn() const { return spawn_; }
    bool spawnValid() const { return spawnValid_; }
    // An automatic crossing re-seats the position only — the traveler
    // walked through the line and keeps looking where they were going.
    bool keepHeading() const { return keepHeading_; }
    void setKeepHeading(bool keep) { keepHeading_ = keep; }

private:
    // The far-water provider of map (mapX, mapZ): cached .twb lakes and
    // ribbons plus the master fleuves, pure and worker-callable (the
    // water system's far layer, and the spawn probe's water oracle).
    render::WaterSystem::FarWaterFn
    makeFarWaterProvider(const SandboxContext& ctx, i32 mapX, i32 mapZ,
                         const render::terraingen::TileBakeParams& bake,
                         const render::terraingen::MapGridSpec& grid,
                         f32 pinHalfWidth) const;
    // Loads the overviews of the cached neighbour maps (3x3 around the
    // active one) that `sb` does not hold yet; returns how many landed.
    u32 loadNeighbourOverviews(
        const SandboxContext& ctx, render::SandboxTerrain& sb,
        const render::terraingen::TileBakeParams& bakeParams) const;
    // Reselects the resident lakes from the registry around `focus`
    // (bbox distance, previous-map residents kept). Returns true when
    // membership changed — the caller republishes the water bodies.
    bool refreshLakes(const Vec3& focus);

    // The active bounded map (saved/restored by the scene).
    i32 mapX_ { 0 };
    i32 mapZ_ { 0 };
    // The tile streamer of the active map (null in the story world).
    uptr<TerrainBakeStreamer> streamer_;
    // The resident water the bakes emitted (rendered/queried through
    // the published WaterBodies).
    vector<render::terraingen::Lake> lakes_;
    vector<render::terraingen::River> rivers_;
    // Map-wide lake registry (every lake of the active map, from the
    // streamer's one-time cache scan). A lake lives in ONE slice's .twb,
    // so per-tile transport misses any lake whose owner slice is out of
    // prefetch reach while the player stands over a far arm — membership
    // is selected from here by distance to the lake's BBOX instead.
    vector<render::terraingen::Lake> mapLakes_;
    bool mapLakesValid_ { false };
    Vec3 lastLakeRefreshFocus_ { 0.0f };
    bool lakeRefreshFocusValid_ { false };
    // The probed start (stable per seed): survives the story camera-init
    // that runs later in the load path.
    Vec3 spawn_ { 0.0f };
    bool spawnValid_ { false };
    bool keepHeading_ { false };
    struct PendingTravel {
        i32 mapX { 0 };
        i32 mapZ { 0 };
        bool hasArrival { false };
        Vec2 arrival { 0.0f, 0.0f };
    };
    std::optional<PendingTravel> pendingTravel_;
    f32 prefetchCooldown_ { 0.0f };
};

} // namespace game
