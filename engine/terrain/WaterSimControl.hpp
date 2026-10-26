#pragma once

#include <glm/glm.hpp>

#include <functional>
#include <string>

#include "engine/core/BakeMailbox.hpp"
#include "engine/core/Defines.hpp"
#include "engine/terrain/TerrainNoise.hpp"
#include "engine/terrain/WaterBodies.hpp"
#include "engine/terrain/WaterSim.hpp"

namespace core {
class JobSystem;
}

namespace render::terrain {

// The real-time sim window's OWNER (docs/PAYSAGE.md §2.3, option C): one
// camera-following shallow-water window stepped on a worker, driven by a
// single phase machine on the main thread. Headless — this class never
// touches the GPU. It owns the CPU state (window, snapshot, session
// cache, breadcrumbs, settle gate, sources, config) and hands the
// renderer, once per update(), an ordered list of GPU requests (upload
// this snapshot, freeze this mesh, clear the frozen meshes) that it
// applies verbatim.
//
// Threading (Phase 5): at most ONE job in flight owns the window state;
// main reads only published snapshots and drains the mailbox at the
// same frame point every frame. Inside its trusted rect the sim owns the
// water per fragment; the baked lakes/ribbons keep everything outside
// and the spin-up fallback.
//
// This is the façade the GAME drives: everything a scene may ask of the
// water sim (sources, ground changes, dump requests, validity queries)
// is here; the renderer only reads it.
class WaterSimControl {
public:
    struct SimConfig {
        bool enabled { true };
        f32 span { 512.0f };
        f32 texel { 2.0f };
        u32 maxSubsteps { 4 }; // hitch clamp per job, at 1x
        // Simulated seconds per real second (0.25x..8x). Per-substep
        // dt never changes (CFL intact): the scale only multiplies
        // how many substeps a real second accumulates — worker cost
        // is linear in it (~1 ms/substep on the 257² window).
        f32 timeScale { 2.0f };
        f32 anchorHysteresis { 16.0f }; // m before the window re-anchors
        f32 fadeBand { 32.0f };         // m of sim->baked edge crossfade
        i32 debugMode { 0 }; // 0 normal, 1 force baked, 2 seam overlay,
                             // 3 volume boxes
        // Settle-gated reveal: after a pre-roll or a cache resume the
        // window keeps SIMULATING but the BAKED water stays on screen
        // until the published volume is calm — the "waterfall
        // restarting from a dry cliff" happens behind the curtain.
        bool settleGated { true };
        // Ribbons at or above this half-width are PINNED (the baked
        // river PLACES its water, the sim animates the margins) —
        // below it, rivers stay 100% sim, fed by entry sources and
        // rain. The threshold is the answer to "does the water
        // renew?": a mid river's real drainage basin spans kilometres
        // the window never sees, so any course wider than a brook
        // must be reservoir-fed (10 m left established rivers nearly
        // dry).
        f32 pinRiverHalfWidth { 4.0f };
        // Rain puddles — under an active storm the still films in
        // terrain dips publish at reduced thresholds (and the kernel
        // rain is storm-boosted). Off = the anti-flicker fallback.
        bool rainPuddles { true };
        WaterSimParams params;
    };

    // ONE phase, written only by simTransition (which also owns the
    // epoch): Off = disabled, no terrain base, or invalidated and not
    // re-entered; AwaitingGround = enabled but the camera's tile is not
    // published yet (sandbox); PreRolling = the equilibrium solve is in
    // flight, no window yet (the settle clock already runs); Settling =
    // the window exists and simulates behind the baked display until
    // its volume is calm or the cap passes; Live = revealed. A window
    // (simState) exists exactly in Settling and Live. Independent of the
    // phase: simGroundDirty (a pending ground refresh), simHasLastCam /
    // simHasLastCrumb (tracking data), simFrozenClearPending (a deferred
    // GPU request) and simJobs.busy() (the worker lane).
    enum class SimPhase : u8 { Off, AwaitingGround, PreRolling, Settling,
                               Live };
    static const char* simPhaseName(SimPhase phase);

    // Boundary-inflow provider (rect -> sources), called ON THE WORKER
    // (pure; the master-network query costs ms). Null = rain only.
    using SimSourcesFn = BoundarySourcesFn;

    // Per-frame inputs. `params` must outlive the call (the jobs copy
    // it); `bodies` is the published local set (null = sea only) and is
    // captured by the jobs, so the pins and entry sources see the set
    // of the frame that kicked them.
    struct Inputs {
        const TerrainParams* params { nullptr };
        sptr<const WaterBodies> bodies;
        Vec3 cameraPos { 0.0f };
        f32 dt { 0.0f }; // real frame seconds
    };

    // What the renderer must do with its GPU resources after update(),
    // in this order. UploadWindow: the live window's snapshot landed —
    // (re)upload the sim textures + mesh. FreezeMesh: keep this
    // snapshot's mesh as a static draw along the travel trail
    // (`replaceRadius` / `pushIfNoMatch` as in simCachePush).
    // ClearFrozenMeshes: the ground changed, every frozen mesh holds
    // old ground.
    struct GpuRequest {
        enum class Kind : u8 { ClearFrozenMeshes, FreezeMesh, UploadWindow };
        Kind kind { Kind::UploadWindow };
        sptr<const WaterSimSnapshot> snap;
        f32 replaceRadius { 0.0f };
        bool pushIfNoMatch { false };
    };

    // The worker lane must exist before the first update().
    void create(core::JobSystem& jobSystem);
    // Orphans the in-flight job (its result dies on arrival), drops the
    // window and every cache; keeps the config, the source provider and
    // the epoch so a destroy()+create() cycle never lets a stale result
    // land as current.
    void destroy();

    // Once per frame, main thread: drains the finished job, applies the
    // policy (gate clock, off/no-base, teleport, dump, knob), enters or
    // resumes a window, kicks the next job. Returns the GPU requests of
    // this frame (valid until the next update()).
    const vector<GpuRequest>& update(const Inputs& in);

    SimConfig& simConfig() { return simCfg; }
    const SimConfig& simConfig() const { return simCfg; }
    // Live storm intensity 0..1 (weather-driven, 0 indoors): scales
    // the kernel rain and arms the puddle publication.
    void setRainIntensity(f32 v) { simRainIntensity = v; }
    void setSimSources(SimSourcesFn fn) { simSourcesFn = std::move(fn); }
    // Gameplay-created sources (springs cast by the player, authored
    // world springs): WORLD space, main-thread owned, replaced
    // wholesale — the scene keeps the truth. Merged into the STEP job
    // only, never into the pre-roll solver (a steady-state solve would
    // eternalize a 10 s spring into a lake behind the curtain). The
    // kernel drops sources outside the window by itself.
    void setSimRuntimeSources(vector<terraingen::WaterSource> sources) {
        simRuntimeSources = std::move(sources);
    }
    // Terraforming hook: the sculpt overlay changed — the next step job
    // re-samples the window's ground and the water reacts live; the
    // cached windows hold pre-sculpt ground, drop them; the frozen
    // meshes too (a ClearFrozenMeshes request on the next update).
    void notifySimGroundChanged() {
        simGroundDirty = true;
        simCache.clear();
        simFrozenClearPending = true;
    }
    // True when the LIVE sim window overlaps the rect — the tile-publish
    // path only forces the ground refresh (66k height() re-samples +
    // cache drop) when a changed region actually reaches the window.
    // No window yet = false (the pre-roll samples fresh ground anyway).
    bool simWindowIntersects(f32 minX, f32 minZ, f32 maxX,
                             f32 maxZ) const {
        if (!simState) {
            return false;
        }
        const auto& spec = simState->spec;
        const f32 span =
            static_cast<f32>(spec.n - 1) * spec.texelSize;
        return spec.originX + span >= minX && spec.originX <= maxX &&
               spec.originZ + span >= minZ && spec.originZ <= maxZ;
    }
    // Drop the window: state, snapshot, accumulator, crumb tracking,
    // then Off (epoch bump) — in-flight results die on arrival. The
    // caller freezes/caches first if the window is worth keeping.
    void simInvalidate(const char* reason);

    // Latest published snapshot (null before the first pre-roll lands)
    // — the gameplay query source inside the trusted rect.
    sptr<const WaterSimSnapshot> simSnapshot() const { return simSnap; }
    // FrameUniforms feed: origin / 1-span / valid (w = 0 until Live).
    Vec4 simMapInfo() const;
    Vec4 simTuneInfo() const {
        return { static_cast<f32>(simCfg.params.marginCells) *
                     simCfg.texel,
                 simCfg.fadeBand, static_cast<f32>(simCfg.debugMode),
                 simCfg.texel }; // w: the debug-box footprint
    }
    f32 simCostMs() const { return simLastMs; } // worker job, F6 line
    // A published snapshot exists (the sim can answer queries); the
    // display additionally waits for the Live phase (settle gate).
    bool simIsValid() const { return simSnap != nullptr; }
    bool simIsPreRolling() const {
        return simPhase == SimPhase::PreRolling;
    }
    bool simIsSettling() const { // gate active
        return simPhase == SimPhase::Settling;
    }
    bool simIsLive() const { return simPhase == SimPhase::Live; }
    SimPhase phase() const { return simPhase; }
    u32 epoch() const { return simEpoch; }
    u32 simWetCellCount() const { return simWetCells; } // last landing
    // On-site debugging: dump the live window (planes + params +
    // sources) to `path` at the next moment main owns the state —
    // `cooker water-replay` then reproduces it offline.
    void requestSimDump(std::string path) {
        simDumpPath = std::move(path);
    }

    // Session LRU capacity of evicted window states; the renderer keeps
    // the same number of frozen meshes.
    static constexpr size_t kSimCacheCap = 4;

private:
    struct SimResult {
        u64 gen { 0 }; // BakeMailbox stamp
        u32 epoch { 0 };
        sptr<WaterSimState> state;
        sptr<WaterSimSnapshot> snap;
        vector<terraingen::WaterSource> sources;
        bool sourcesFresh { false };
        f32 millis { 0.0f };
        f64 volume { 0.0 }; // published m³ (settle-gate calm metric)
    };

    // The only writer of simPhase and simEpoch: entering Off from any
    // other phase bumps the epoch so in-flight results die on arrival.
    void simTransition(SimPhase next, const char* why);
    f32 simWindowTexel() const { return glm::max(simCfg.texel, 0.5f); }
    // The window rect centred on the camera, origin snapped to texels.
    terraingen::GridSpec simWindowSpecAt(const Vec3& cameraPos) const;
    // Reset the settle-gate clock and calm metric (every (re)entry).
    void simArmSettleGate();
    // update() in four steps, in this order each frame:
    // 1. land finished jobs (stale epochs are cached + frozen);
    void simDrainResults();
    // 2. gate clock + cap reveal, off/no-base, teleport, busy, dump,
    //    knob — false = nothing more to do this frame;
    bool simApplyPolicy(const Inputs& in);
    // 3. no window: wait for final ground, resume a cached one (true:
    //    tick it now) or kick the pre-roll (false);
    bool simMaybeEnter(const Inputs& in);
    // 4. the regular tick: accumulator, re-anchor, crumbs, step job.
    void simTick(const Inputs& in);
    // `replaceRadius` = how close an existing entry's origin must be
    // to be SUPERSEDED; `pushIfNoMatch` = false makes it a pure
    // in-place refresh (never grows the cache, never evicts the
    // trail). The TIMED refresh must use tight/replace-only: with the
    // travel-wide radius it replaced the previous entry every few
    // seconds of walking — ONE entry slid along with the player
    // instead of a trail, and the waterfall behind vanished past
    // ~half a window.
    void simCachePush(sptr<WaterSimState> state, f32 replaceRadius,
                      bool pushIfNoMatch);
    // Queue a FreezeMesh request (the renderer mirrors simCachePush on
    // its frozen meshes).
    void simFreeze(const sptr<const WaterSimSnapshot>& snap,
                   f32 replaceRadius, bool pushIfNoMatch);

    core::BakeMailbox<SimResult> simJobs;
    vector<GpuRequest> gpuRequests; // this frame's, in order

    SimConfig simCfg;
    SimSourcesFn simSourcesFn;
    SimPhase simPhase { SimPhase::Off };
    sptr<WaterSimState> simState;
    sptr<const WaterSimSnapshot> simSnap;
    vector<terraingen::WaterSource> simSrcCache;
    vector<terraingen::WaterSource> simRuntimeSources; // gameplay springs
    bool simGroundDirty { false };
    bool simFrozenClearPending { false };
    f32 simRainIntensity { 0.0f }; // live storm 0..1
    u32 simEpoch { 0 };            // bumped on every entry into Off
    f32 simAccum { 0.0f };
    Vec2 simLastCam { 0.0f, 0.0f };
    bool simHasLastCam { false };
    f32 simLastMs { 0.0f };
    u32 simWetCells { 0 };
    // Session LRU of evicted window states (~2-3 MB each): a teleport
    // back into a zone RESUMES its water via scrollWindow instead of
    // re-running the pre-roll solver — no visible re-settling. Never
    // serialized (§2.4); cleared on sculpt and destroy. Back = most
    // recent.
    vector<sptr<WaterSimState>> simCache;
    // Breadcrumbs: while the window SCROLLS away (no teleport, so no
    // eviction ever happens), a COPY of the state is dropped into the
    // cache every half-window of travel — scrollWindow refills its
    // entering strips from them, so walking back to a waterfall finds
    // it flowing instead of re-forming from dry strips.
    Vec2 simLastCrumb { 0.0f, 0.0f };
    bool simHasLastCrumb { false };
    // Purely DYNAMIC water (a spread pool, a flood film) exists in no
    // baked body: it only survives eviction through the cache and the
    // frozen meshes. Crumb/freeze drops tied to TRAVEL alone missed
    // any water formed since the last drop (it vanished at the margin
    // ring and was forgotten on return) — so the CURRENT footprint's
    // crumb + frozen mesh are also refreshed on a timer (the footprint
    // dedup replaces in place; the trail stays).
    f32 simFreshTimer { 0.0f };
    static constexpr f32 kSimFreshSeconds = 4.0f;
    // Replace-only radius of the timed refresh (see simCachePush).
    static constexpr f32 kSimFreshRadius = 64.0f;
    // Settle-gated reveal (SimConfig::settleGated, the Settling phase):
    // the window simulates behind the baked display until the
    // published volume is calm for kSimCalmTicks consecutive results
    // (relative delta under kSimCalmEps) or kSimSettleCap seconds have
    // passed since the (re)entry.
    u32 simCalmTicks { 0 };
    f64 simLastVolume { -1.0 };
    f32 simSettleTimer { 0.0f };
    static constexpr f32 kSimCalmEps = 2.0e-3f;
    static constexpr u32 kSimCalmTicks = 8;
    static constexpr f32 kSimSettleCap = 3.0f; // s, never blocks longer
    std::string simDumpPath; // non-empty = dump requested
};

} // namespace render::terrain
