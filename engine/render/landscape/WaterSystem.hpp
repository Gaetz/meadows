#pragma once

#include <glm/glm.hpp>

#include "engine/core/ConcurrentQueue.hpp"
#include "engine/core/Defines.hpp"
#include "engine/render/landscape/TerrainNoise.hpp"
#include "engine/rhi/Rhi.hpp"
#include "engine/terrain/WaterBodies.hpp"

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
    struct Shared {
        core::ConcurrentQueue<BakedMap> baked;
        core::ConcurrentQueue<BakedInfo> bakedInfo;
    };

    void buildPipeline(rhi::Device& device, ShaderLibrary& shaders);
    void rebuildLocalGeometry(rhi::Device& device);
    void rebuildMapGroup(rhi::Device& device);
    void rebuildMaterials(rhi::Device& device);

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
    u64 shaderGeneration { 0 };
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
};

} // namespace render
