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

    // Water-info map (engine/terrain/WaterInfoMap): camera-local surface
    // height / depth / composited flow for lakes+rivers — the per-pixel
    // junction resolution. Same worker-bake mailbox as the pool map,
    // tighter follow distance (finer texels).
    static constexpr u32 kInfoMapSize = 1024;
    static constexpr f32 kInfoMapSpan = 1536.0f;
    static constexpr f32 kInfoRebakeDistance = 384.0f;

    void create(rhi::Device& device, ShaderLibrary& shaders,
                core::JobSystem& jobSystem);
    void destroy(rhi::Device& device);
    void refreshPipeline(rhi::Device& device, ShaderLibrary& shaders);

    // Pumps finished pool-map bakes and triggers a rebake when the camera
    // strays or sea level / seed changed. Main thread, once per frame.
    void update(rhi::Device& device, const TerrainParams& params,
                const Vec3& cameraPos);

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
    // For FrameUniforms::waterInfoMapInfo (xy = center, z = 1/span,
    // w = valid). Invalid while the first bake runs or right after the
    // bodies changed — the shader then falls back to pure vertex data,
    // so a stale map can never show wrong junctions.
    Vec4 infoMapInfo() const {
        return { infoCenter.x, infoCenter.y, 1.0f / kInfoMapSpan,
                 infoValid ? 1.0f : 0.0f };
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
        u32 maxSubsteps { 4 };       // hitch clamp per job
        f32 anchorHysteresis { 16.0f }; // m before the window re-anchors
        f32 fadeBand { 32.0f };         // m of sim->baked edge crossfade
        i32 debugMode { 0 }; // 0 normal, 1 force baked, 2 seam overlay
        // Settle-gated reveal: after a pre-roll or a cache resume the
        // window keeps SIMULATING but the BAKED water stays on screen
        // until the published volume is calm — the "waterfall
        // restarting from a dry cliff" happens behind the curtain.
        bool settleGated { true };
        terrain::WaterSimParams params;
    };
    SimConfig& simConfig() { return simCfg; }
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
    struct BakedInfo {
        Vec2 center {};
        u64 generation { 0 };
        u32 seed { 0 };
        u64 bodiesStamp { 0 };
        vector<f32> surface; // R32F payload
        vector<f32> extras;  // RGBA16F payload: depth, flowXZ, spare
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
    struct Shared {
        core::ConcurrentQueue<BakedMap> baked;
        core::ConcurrentQueue<BakedInfo> bakedInfo;
        core::ConcurrentQueue<SimResult> simDone;
    };

    void buildPipeline(rhi::Device& device, ShaderLibrary& shaders);
    void rebuildLocalGeometry(rhi::Device& device,
                              const TerrainParams& params);
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

    // Water-info map state (bindings 5/6 of poolMapGroup).
    bool infoBakeInFlight { false };
    bool infoValid { false };
    Vec2 infoCenter { 1.0e9f, 1.0e9f };
    u32 infoSeed { 0 };
    u64 infoBodiesStamp { ~0ull };
    rhi::TextureHandle infoMapA {};
    rhi::TextureHandle infoMapB {};
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
    rhi::BufferHandle localVertexBuffer {};
    rhi::BufferHandle localIndexBuffer {};
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
    void simCachePush(sptr<terrain::WaterSimState> state);
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
                   const terrain::WaterSimSnapshot& snap);
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
