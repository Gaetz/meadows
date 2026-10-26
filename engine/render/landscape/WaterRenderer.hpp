#pragma once

#include <glm/glm.hpp>

#include <array>
#include <functional>

#include "engine/core/BakeMailbox.hpp"
#include "engine/core/Defines.hpp"
#include "engine/render/landscape/HeightField.hpp"
#include "engine/render/WatchedPipeline.hpp"
#include "engine/rhi/Rhi.hpp"
#include "engine/rhi/UniqueHandle.hpp"
#include "engine/terrain/TerrainNoise.hpp"
#include "engine/terrain/WaterBodies.hpp"
#include "engine/terrain/WaterGeometry.hpp"
#include "engine/terrain/WaterSim.hpp"
#include "engine/terrain/WaterSimControl.hpp"

namespace core {
class JobSystem;
}
namespace rhi {
class CommandBuffer;
class Device;
}

namespace render {

class ShaderLibrary;

// The GPU half of the water (docs/RENDERING.md): the sea quad following
// the camera, the baked local lakes/ribbons and the far sheets beyond
// the streamed tiles, the pool-depth map the foam reads, the water
// material table, and the sim window's textures + meshes (live, frozen
// trail, debug volumes). It owns pipelines, buffers, textures and bind
// groups, and the three worker lanes whose payload it uploads (pool
// map, local mesh, far mesh).
//
// It never decides anything about the SIM: it reads the headless
// terrain::WaterSimControl (phase, snapshot, config) and applies the GPU
// requests that control hands over each frame, in order.
class WaterRenderer {
public:
    // Pool-depth map: vertical water depth (sea level minus terrain height)
    // baked on a WORKER around the camera, pre-dilated (neighborhood max).
    // The foam shader reads one view-independent tap from it — screen-
    // space probes made foam appear/vanish with camera distance.
    static constexpr u32 kPoolMapSize = 256;
    static constexpr f32 kPoolMapSpan = 3072.0f; // meters covered
    static constexpr f32 kRebakeDistance = 512.0f;
    // Water material presets (WaterMaterialsUbo, binding 1 of the map
    // group): 16 slots, slot 0 = default water. Mirrored to GLSL by the
    // frame constants preamble (FrameLanes.hpp).
    static constexpr u32 kMaxWaterMaterials = terrain::kMaxWaterMaterials;

    void create(rhi::Device& device, ShaderLibrary& shaders,
                core::JobSystem& jobSystem);
    void destroy(rhi::Device& device);
    // Builds the pipelines on first call and rebuilds the ones whose
    // shaders were hot-reloaded (WatchedPipeline). Once per frame.
    void refreshPipeline(rhi::Device& device, ShaderLibrary& shaders);

    // Pumps finished pool-map bakes and triggers a rebake when the camera
    // strays or sea level / seed changed. Main thread, once per frame.
    // `field` (nullable): the shared height pyramid — the pool bake
    // samples it instead of pointwise terrain::height (exact when null).
    void update(rhi::Device& device, const TerrainParams& params,
                const Vec3& cameraPos,
                sptr<const HeightField::Snapshot> field = nullptr);

    // Far water: flat lake sheets + river ribbons for the world BEYOND
    // the streamed tiles, drawn with the local pipeline (opaque,
    // depth-written — wherever near water exists it wins the depth
    // test, so no cut bookkeeping). The provider runs ON A WORKER
    // (pure: cached .twb reads + the memoized master network) and
    // returns everything in the requested square.
    using FarWaterSet = terrain::FarWaterSet;
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
    // The same set, shared: the sim jobs capture it.
    const sptr<const WaterBodies>& bodiesRef() const { return bodies; }

    // For FrameUniforms::waterMapInfo (xy = map center, z = 1/span).
    Vec4 poolMapInfo() const {
        return { mapCenter.x, mapCenter.y, 1.0f / kPoolMapSpan, 0.0f };
    }

    // Apply the sim control's GPU requests of this frame, in order,
    // right after its update(): upload the landed snapshot (textures +
    // the ONE closed mesh + debug volumes when `cfg.debugMode` shows
    // them), freeze a mesh along the trail, clear the frozen trail.
    void applySimRequests(
        rhi::Device& device,
        const vector<terrain::WaterSimControl::GpuRequest>& requests,
        const terrain::WaterSimControl::SimConfig& cfg);

    // Frozen windows lane (oldest -> newest; z = 1/span, 0 = empty).
    // Zeroed in force-baked mode so the baked never yields to meshes
    // that are not drawn.
    std::array<Vec4, 4> simFrozenInfo(i32 debugMode) const;

    // `sceneBindGroup` holds the pre-water scene color+depth snapshot
    // (texture units 0 and 1) — owned by the scene, since the snapshot
    // textures track the window size. `sim` decides whether the live
    // sheet shows (Live phase, a snapshot, not force-baked).
    void draw(rhi::CommandBuffer& cmd, rhi::BindGroupHandle frameBindGroup,
              rhi::BindGroupHandle sceneBindGroup,
              const terrain::WaterSimControl& sim);

private:
    struct BakedMap {
        Vec2 center {};
        u64 gen { 0 }; // BakeMailbox stamp
        u32 seed { 0 };
        f32 seaLevel { 0.0f };
        u64 bodiesStamp { 0 };
        u64 contentStamp { 0 }; // rect-scoped via contentTouchedSince
        vector<f32> texels;
    };
    // Local (lakes + ribbons) geometry, built on a WORKER: the build
    // is heavy (per-texel lake quads with perimeter probes, ribbons
    // densified to 4 m with a terrain::height per node) and it runs
    // on EVERY streamed tile — synchronous on main it stuttered the
    // frame through every bake storm. Main only uploads the buffers
    // (Phase 5).
    struct LocalMesh {
        u64 gen { 0 }; // BakeMailbox stamp
        u64 stamp { 0 }; // bodiesStamp the build saw
        vector<f32> verts;
        vector<u32> indices;
    };
    // Far-water geometry, built like LocalMesh on a worker; carries
    // the request center + content stamp for the staleness check.
    struct FarMesh {
        u64 gen { 0 }; // BakeMailbox stamp
        u64 stamp { 0 }; // contentStamp the build saw
        Vec2 center { 0.0f };
        vector<f32> verts;
        vector<u32> indices;
    };
    void kickLocalGeometry(const TerrainParams& params);
    void rebuildMapGroup(rhi::Device& device);
    void rebuildMaterials(rhi::Device& device);
    void uploadSimTextures(rhi::Device& device,
                           const terrain::WaterSimSnapshot& snap,
                           i32 debugMode);

    // The three worker lanes (one job in flight each, generation-stamped
    // results drained on main): the pool-depth map, the local and far
    // geometry builds.
    core::BakeMailbox<BakedMap> poolBakes;
    core::BakeMailbox<LocalMesh> localBuilds;
    core::BakeMailbox<FarMesh> farBuilds;
    Vec2 mapCenter { 1e9f, 1e9f }; // far away: first update() bakes
    u32 bakedSeed { 0 };
    f32 bakedSeaLevel { -1e9f };

    rhi::UniqueBuffer vertexBuffer;
    rhi::UniqueBuffer indexBuffer;
    WatchedPipeline pipeline;
    rhi::UniqueTexture poolMap;
    rhi::UniqueSampler poolMapSampler;
    // Sim window depth / current XZ / spare (binding 8 of the map group).
    rhi::UniqueTexture simMapB;

    rhi::UniqueBuffer materialsUbo;
    // Declared after everything it binds: members destroy in reverse
    // order, so the group goes before its textures and buffers.
    rhi::UniqueBindGroup poolMapGroup;

    // Local surfaces (lakes/rivers): world-space triangles, own pipeline
    // (waterlocal.vert), shared water shading.
    sptr<const WaterBodies> bodies;
    u64 bodiesStamp { 0 };
    u64 bakedBodiesStamp { ~0ull };
    u64 bakedContentStamp { 0 }; // pool map, rect-scoped (touchedSince)
    bool bodiesDirty { false };
    rhi::UniqueBuffer localVertexBuffer;
    rhi::UniqueBuffer localIndexBuffer;
    FarWaterFn farFn;
    bool farDirty { false };
    Vec2 farCenter { 1.0e9f, 1.0e9f };
    u64 farStamp { ~0ull };
    core::QuietStamp farContent; // the far collect rescans once per publish burst
    rhi::UniqueBuffer farVertexBuffer;
    rhi::UniqueBuffer farIndexBuffer;
    u32 farIndexCount { 0 };
    WatchedPipeline localPipeline;
    u32 localIndexCount { 0 };

    // Frozen windows: at every crumb/teleport push of the sim control,
    // the current snapshot MESH is kept as a static draw — the sim
    // stays visible (frozen) beyond the live rect along the travel
    // trail. From afar frozen water is indistinguishable (waves are
    // LOD-flattened, advection invisible); the alternative — growing
    // the window — scales n² in kernel, uploads and pre-roll.
    struct FrozenWindow {
        rhi::UniqueBuffer vertexBuffer;
        rhi::UniqueBuffer indexBuffer;
        u32 indexCount { 0 };
        f32 originX { 0.0f };
        f32 originZ { 0.0f };
        f32 span { 0.0f };
    };
    vector<FrozenWindow> simFrozen; // oldest -> newest
    void simFreeze(rhi::Device& device,
                   const terrain::WaterSimSnapshot& snap,
                   f32 replaceRadius, bool pushIfNoMatch);
    void simFrozenClearNow();
    WatchedPipeline simFrozenPipeline;
    // The ONE closed water mesh (built worker-side in extractSnapshot,
    // uploaded verbatim per tick).
    rhi::UniqueBuffer simVertexBuffer;
    rhi::UniqueBuffer simIndexBuffer;
    u32 simIndexCount { 0 };
    WatchedPipeline simPipeline;
    // Seam-overlay variant: no depth test — shows where the sim HAS
    // water even where the sheet would lose the depth fight (the
    // unambiguous "is the sim alive here" view).
    WatchedPipeline simPipelineOverlay;
    // Debug volume boxes (Sim mode "Volumes debug"): one translucent
    // column per wet cell, instanced.
    rhi::UniqueBuffer simBoxVertexBuffer;
    rhi::UniqueBuffer simBoxIndexBuffer;
    rhi::UniqueBuffer simBoxInstanceBuffer;
    u32 simBoxInstances { 0 };
    WatchedPipeline simBoxPipeline;
};

} // namespace render
