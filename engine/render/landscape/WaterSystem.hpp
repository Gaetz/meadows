#pragma once

#include <glm/glm.hpp>

#include <array>
#include <functional>
#include <string>

#include "engine/core/ConcurrentQueue.hpp"
#include "engine/core/Defines.hpp"
#include "engine/render/landscape/TerrainNoise.hpp"
#include "engine/rhi/Rhi.hpp"
#include "engine/terrain/WaterBodies.hpp"
#include "engine/terrain/WaterSim.hpp"
#include "engine/render/ShaderLibrary.hpp"

namespace core {
class JobSystem;
}
namespace rhi {
class CommandBuffer;
class Device;
}

namespace render {

class ShaderLibrary;

// Water surface (docs/RENDERING.md): one large quad at sea level following the camera
// (snapped to the chunk grid), shaded per pixel — procedural scrolling wave
// normals, fresnel between a PLANAR reflection
// and a REFRACTED scene color (sampled from the pre-water scene
// snapshot, distorted by the waves, absorbed with depth), plus depth-based
// shore foam. Renders into the HDR target after the opaque pass, depth-tested
// against it (terrain above sea level occludes normally).
class WaterSystem {
public:
    // Pool-depth map: vertical water depth (sea level minus terrain height)
    // baked on a WORKER around the camera, pre-dilated (neighborhood max).
    // The foam shader reads one view-independent tap from it — the earlier
    // screen-space probes made foam appear/vanish with camera distance.
    static constexpr u32 kPoolMapSize = 256;
    static constexpr f32 kPoolMapSpan = 3072.0f; // meters covered
    static constexpr f32 kRebakeDistance = 512.0f;

    void create(rhi::Device& device, ShaderLibrary& shaders,
                core::JobSystem& jobSystem);
    void destroy(rhi::Device& device);
    void refreshPipeline(rhi::Device& device, ShaderLibrary& shaders);

    // Pumps finished pool-map bakes and triggers a rebake when the camera
    // strays or sea level / seed changed. Main thread, once per frame.
    void update(rhi::Device& device, const TerrainParams& params,
                const Vec3& cameraPos);

    // Far water (E4a): flat lake sheets + river ribbons for the world
    // BEYOND the streamed tiles, drawn with the local pipeline (opaque,
    // depth-written — wherever near water exists it wins the depth
    // test, so no cut bookkeeping). The provider runs ON A WORKER
    // (pure: cached .twb reads + the memoized master network) and
    // returns everything in the requested square.
    struct FarWaterSet {
        struct Lake {
            f32 level { 0.0f };
            f32 minX { 0.0f };
            f32 minZ { 0.0f };
            f32 cell { 64.0f }; // coarse mask texel (m)
            u32 w { 0 };
            u32 h { 0 };
            vector<u8> mask;
        };
        struct Ribbon {
            struct Node {
                f32 x { 0.0f };
                f32 z { 0.0f };
                f32 surface { 0.0f };
                f32 halfWidth { 0.0f };
            };
            vector<Node> nodes;
        };
        vector<Lake> lakes;
        vector<Ribbon> ribbons;
    };
    using FarWaterFn =
        std::function<FarWaterSet(f32 cx, f32 cz, f32 halfSpan)>;
    void setFarWater(FarWaterFn fn) {
        farFn = std::move(fn);
        farDirty = true;
    }

    // Local water bodies (altitude lakes + river ribbons): the scene
    // publishes an immutable set; geometry rebuilds and the pool map
    // rebakes (foam then works on lakes/rivers too). Null = sea only.
    void setBodies(sptr<const WaterBodies> next);
    // The published set (null = sea only) — the submersion overlay
    // queries it so being under a LAKE tints like being under the sea.
    const WaterBodies* currentBodies() const { return bodies.get(); }

    // For FrameUniforms::waterMapInfo (xy = map center, z = 1/span).
    Vec4 poolMapInfo() const {
        return { mapCenter.x, mapCenter.y, 1.0f / kPoolMapSpan, 0.0f };
    }
    // `sceneBindGroup` holds the pre-water scene color+depth snapshot
    // (texture units 0 and 1) — owned by the scene, since the snapshot
    // textures track the window size.
    void draw(rhi::CommandBuffer& cmd, rhi::BindGroupHandle frameBindGroup,
              rhi::BindGroupHandle sceneBindGroup);

    // --- Real-time sim window (option C, engine/terrain/WaterSim) ----
    // One camera-following shallow-water window stepped on a worker
    // (Phase-5: at most ONE job in flight owns the state; main reads
    // only published snapshots). Inside its trusted rect the sim owns
    // the water per fragment; the baked lakes/ribbons keep everything
    // outside and the spin-up fallback.
    struct SimConfig {
        bool enabled { true };
        f32 span { 512.0f };
        f32 texel { 2.0f };
        u32 maxSubsteps { 4 };       // hitch clamp per job, at 1x
        // Simulated seconds per real second (0.25x..8x). Per-substep
        // dt never changes (CFL intact): the scale only multiplies
        // how many substeps a real second accumulates — worker cost
        // is linear in it (~1 ms/substep on the 257² window).
        f32 timeScale { 2.0f }; // dev default: 2x reads better in-game
        f32 anchorHysteresis { 16.0f }; // m before the window re-anchors
        f32 fadeBand { 32.0f };         // m of sim->baked edge crossfade
        i32 debugMode { 0 }; // 0 normal, 1 force baked, 2 seam overlay
        // Settle-gated reveal: after a pre-roll or a cache resume the
        // window keeps SIMULATING but the BAKED water stays on screen
        // until the published volume is calm — the "waterfall
        // restarting from a dry cliff" happens behind the curtain.
        bool settleGated { true };
        // E1b: ribbons at or above this half-width are PINNED (the
        // baked river PLACES its water, the sim animates the margins)
        // — below it, rivers stay 100% sim, fed by entry sources and
        // rain. The threshold is the answer to "does the water
        // renew?": a mid river's real drainage basin spans kilometres
        // the window never sees, so any course wider than a brook
        // must be reservoir-fed (dev: 10 m left established rivers
        // nearly dry).
        f32 pinRiverHalfWidth { 4.0f };
        // E6: rain puddles — under an active storm the still films in
        // terrain dips publish at reduced thresholds (and the kernel
        // rain is storm-boosted). Off = the anti-flicker fallback.
        bool rainPuddles { true };
        terrain::WaterSimParams params;
    };
    SimConfig& simConfig() { return simCfg; }
    const SimConfig& simConfig() const { return simCfg; }
    // Live storm intensity 0..1 (weather-driven, 0 indoors): scales
    // the kernel rain and arms the E6 puddle publication.
    void setRainIntensity(f32 v) { simRainIntensity = v; }
    // Boundary-inflow provider (rect -> sources), called ON THE WORKER
    // (pure; the master-network query costs ms). Null = rain only.
    using SimSourcesFn =
        std::function<vector<terraingen::WaterSource>(f32, f32, f32,
                                                      f32)>;
    void setSimSources(SimSourcesFn fn) { simSourcesFn = std::move(fn); }
    // Terraforming hook: the sculpt overlay changed — the next step job
    // re-samples the window's ground and the water reacts live; the
    // cached windows hold pre-sculpt ground, drop them.
    void notifySimGroundChanged() {
        simGroundDirty = true;
        simCache.clear();
        simFrozenClearPending = true; // frozen meshes hold old ground
    }
    // Once per frame, after update(). dt = real frame seconds.
    void updateSim(rhi::Device& device, const TerrainParams& params,
                   const Vec3& cameraPos, f32 dt);
    // Latest published snapshot (null before the first pre-roll lands)
    // — the gameplay query source inside the trusted rect.
    sptr<const terrain::WaterSimSnapshot> simSnapshot() const {
        return simSnap;
    }
    // FrameUniforms feeds: origin/1-span/valid + trusted/fade/debug.
    Vec4 simMapInfo() const;
    // Frozen windows lane (oldest -> newest; z = 1/span, 0 = empty).
    // Zeroed in force-baked mode so the baked never yields to meshes
    // that are not drawn.
    std::array<Vec4, 4> simFrozenInfo() const;
    Vec4 simTuneInfo() const {
        return { static_cast<f32>(simCfg.params.marginCells) *
                     simCfg.texel,
                 simCfg.fadeBand, static_cast<f32>(simCfg.debugMode),
                 simCfg.texel }; // w: the debug-box footprint
    }
    f32 simCostMs() const { return simLastMs; } // worker job, F6 line
    bool simIsValid() const { return simValid; }
    bool simIsPreRolling() const { return simInFlight && !simState; }
    bool simIsSettling() const { return simSettling; } // gate active
    u32 simWetCellCount() const { return simWetCells; } // last upload
    // On-site debugging: dump the live window (planes + params +
    // sources) to `path` at the next moment main owns the state —
    // `cooker water-replay` then reproduces it offline.
    void requestSimDump(std::string path) {
        simDumpPath = std::move(path);
    }

private:
    struct BakedMap {
        Vec2 center {};
        u64 generation { 0 };
        u32 seed { 0 };
        f32 seaLevel { 0.0f };
        u64 bodiesStamp { 0 };
        vector<f32> texels;
    };
    // Local (lakes + ribbons) geometry, built on a WORKER: the build
    // grew heavy (per-texel lake quads with perimeter probes, ribbons
    // densified to 4 m with a terrain::height per node) and it runs
    // on EVERY streamed tile — synchronous on main it stuttered the
    // frame through every bake storm (measured dev). Main only
    // uploads the buffers (Phase-5).
    struct LocalMesh {
        u64 generation { 0 };
        u64 stamp { 0 }; // bodiesStamp the build saw
        vector<f32> verts;
        vector<u32> indices;
    };
    struct SimResult {
        u64 generation { 0 };
        u32 epoch { 0 };
        sptr<terrain::WaterSimState> state;
        sptr<terrain::WaterSimSnapshot> snap;
        vector<terraingen::WaterSource> sources;
        bool sourcesFresh { false };
        f32 millis { 0.0f };
        f64 volume { 0.0 }; // published m³ (settle-gate calm metric)
    };
    // Far-water geometry, built like LocalMesh on a worker; carries
    // the request center + content stamp for the staleness check.
    struct FarMesh {
        u64 generation { 0 };
        u64 stamp { 0 }; // contentStamp the build saw
        Vec2 center { 0.0f };
        vector<f32> verts;
        vector<u32> indices;
    };
    struct Shared {
        core::ConcurrentQueue<BakedMap> baked;
        core::ConcurrentQueue<SimResult> simDone;
        core::ConcurrentQueue<LocalMesh> localMesh;
        core::ConcurrentQueue<FarMesh> farMesh;
    };

    void buildPipeline(rhi::Device& device, ShaderLibrary& shaders);
    void kickLocalGeometry(const TerrainParams& params);
    // Worker-side geometry build (see LocalMesh): pure function of
    // the published bodies + params, static so the job needs no this.
    static void buildLocalGeometry(const WaterBodies& bodies,
                                   const TerrainParams& params,
                                   vector<f32>& verts,
                                   vector<u32>& indices);
    // Far sheets/ribbons in the LOCAL vertex format (still-water
    // lanes): drawn with localPipeline, shaded like any baked water.
    static void buildFarGeometry(const FarWaterSet& set,
                                 vector<f32>& verts,
                                 vector<u32>& indices);
    void rebuildMapGroup(rhi::Device& device);
    void rebuildMaterials(rhi::Device& device);
    void uploadSimTextures(rhi::Device& device,
                           const terrain::WaterSimSnapshot& snap);

    sptr<Shared> shared;
    core::JobSystem* jobs { nullptr };
    u64 generation { 0 };
    bool bakeInFlight { false };
    Vec2 mapCenter { 1e9f, 1e9f }; // far away: first update() bakes
    u32 bakedSeed { 0 };
    f32 bakedSeaLevel { -1e9f };

    rhi::BufferHandle vertexBuffer {};
    rhi::BufferHandle indexBuffer {};
    rhi::PipelineHandle pipeline {};
    ShaderLibrary::Watch shaderWatch;
    rhi::TextureHandle poolMap {};
    rhi::SamplerHandle poolMapSampler {};
    rhi::BindGroupHandle poolMapGroup {};

    // Water material presets (WaterMaterialsUbo, binding 1 of the map
    // group): 16 slots, slot 0 = default water.
    static constexpr u32 kMaxWaterMaterials = 16;
    rhi::BufferHandle materialsUbo {};

    // Local surfaces (lakes/rivers): world-space triangles, own pipeline
    // (waterlocal.vert), shared water shading.
    sptr<const WaterBodies> bodies;
    u64 bodiesStamp { 0 };
    u64 bakedBodiesStamp { ~0ull };
    bool bodiesDirty { false };
    bool localBuildInFlight { false };
    rhi::BufferHandle localVertexBuffer {};
    rhi::BufferHandle localIndexBuffer {};
    FarWaterFn farFn;
    bool farDirty { false };
    bool farBuildInFlight { false };
    Vec2 farCenter { 1.0e9f, 1.0e9f };
    u64 farStamp { ~0ull };
    rhi::BufferHandle farVertexBuffer {};
    rhi::BufferHandle farIndexBuffer {};
    u32 farIndexCount { 0 };
    rhi::PipelineHandle localPipeline {};
    u32 localIndexCount { 0 };

    // Sim window state (see the public block).
    SimConfig simCfg;
    SimSourcesFn simSourcesFn;
    sptr<terrain::WaterSimState> simState;
    sptr<const terrain::WaterSimSnapshot> simSnap;
    vector<terraingen::WaterSource> simSrcCache;
    bool simInFlight { false };
    bool simGroundDirty { false };
    bool simValid { false }; // snapshot uploaded and fresh
    f32 simRainIntensity { 0.0f }; // live storm 0..1 (E6 puddles)
    u32 simEpoch { 0 };      // bumped on invalidation (fly mode, off)
    f32 simAccum { 0.0f };
    Vec2 simLastCam { 0.0f, 0.0f };
    bool simHasLastCam { false };
    f32 simLastMs { 0.0f };
    // Session LRU of evicted window states (~2-3 MB each): a teleport
    // back into a zone RESUMES its water via scrollWindow instead of
    // re-running the pre-roll solver — no visible re-settling. Never
    // serialized (§2.4); cleared on sculpt, bodies change, reset.
    // Back = most recent.
    static constexpr size_t kSimCacheCap = 4;
    vector<sptr<terrain::WaterSimState>> simCache;
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
    // ring and was forgotten on return, measured dev) — so the
    // CURRENT footprint's crumb + frozen mesh are also refreshed on a
    // timer (the footprint dedup replaces in place; the trail stays).
    f32 simFreshTimer { 0.0f };
    static constexpr f32 kSimFreshSeconds = 4.0f;
    // Replace-only radius of the timed refresh (see simCachePush).
    static constexpr f32 kSimFreshRadius = 64.0f;
    // Settle-gated reveal (SimConfig::settleGated): true while the
    // window simulates behind the baked display. Cleared when the
    // published volume is calm for kSimCalmTicks consecutive results
    // (relative delta under kSimCalmEps) or after simSettleCap
    // seconds.
    bool simSettling { false };
    u32 simCalmTicks { 0 };
    f64 simLastVolume { -1.0 };
    f32 simSettleTimer { 0.0f };
    static constexpr f32 kSimCalmEps = 2.0e-3f;
    static constexpr u32 kSimCalmTicks = 8;
    static constexpr f32 kSimSettleCap = 3.0f; // s, never blocks longer
    // `replaceRadius` = how close an existing entry's origin must be
    // to be SUPERSEDED; `pushIfNoMatch` = false makes it a pure
    // in-place refresh (never grows the cache, never evicts the
    // trail). The TIMED refresh must use tight/replace-only: with the
    // travel-wide radius it replaced the previous entry every few
    // seconds of walking — ONE entry slid along with the player
    // instead of a trail, and the waterfall behind vanished past
    // ~half a window (measured dev).
    void simCachePush(sptr<terrain::WaterSimState> state,
                      f32 replaceRadius, bool pushIfNoMatch);
    // Frozen windows: at every crumb/teleport push, the current
    // snapshot MESH is also kept as a static draw — the sim stays
    // visible (frozen) beyond the live rect along the travel trail.
    // From afar frozen water is indistinguishable (waves are
    // LOD-flattened, advection invisible); the alternative — growing
    // the window — scales n² in kernel, uploads and pre-roll.
    struct FrozenWindow {
        rhi::BufferHandle vertexBuffer {};
        rhi::BufferHandle indexBuffer {};
        u32 indexCount { 0 };
        f32 originX { 0.0f };
        f32 originZ { 0.0f };
        f32 span { 0.0f };
    };
    vector<FrozenWindow> simFrozen; // oldest -> newest
    bool simFrozenClearPending { false };
    void simFreeze(rhi::Device& device,
                   const terrain::WaterSimSnapshot& snap,
                   f32 replaceRadius, bool pushIfNoMatch);
    void simFrozenClearNow(rhi::Device& device);
    rhi::PipelineHandle simFrozenPipeline {};
    rhi::TextureHandle simMapA {};
    rhi::TextureHandle simMapB {};
    // The ONE closed water mesh (built worker-side in extractSnapshot,
    // uploaded verbatim per tick).
    rhi::BufferHandle simVertexBuffer {};
    rhi::BufferHandle simIndexBuffer {};
    u32 simIndexCount { 0 };
    u32 simWetCells { 0 };
    std::string simDumpPath; // non-empty = dump requested
    rhi::PipelineHandle simPipeline {};
    // Seam-overlay variant: no depth test — shows where the sim HAS
    // water even where the sheet would lose the depth fight (the
    // unambiguous "is the sim alive here" view).
    rhi::PipelineHandle simPipelineOverlay {};
    // Debug volume boxes (Sim mode "Volumes debug"): one translucent
    // column per wet cell, instanced.
    rhi::BufferHandle simBoxVertexBuffer {};
    rhi::BufferHandle simBoxIndexBuffer {};
    rhi::BufferHandle simBoxInstanceBuffer {};
    u32 simBoxInstances { 0 };
    rhi::PipelineHandle simBoxPipeline {};
};

} // namespace render
