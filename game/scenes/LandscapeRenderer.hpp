#pragma once

#include <unordered_set>

#include "engine/core/FrameProbe.hpp"
#include "engine/render/Camera3D.hpp"
#include "engine/render/ShaderLibrary.hpp"
#include "engine/render/landscape/ChunkOcclusion.hpp"
#include "engine/render/landscape/GpuOcclusion.hpp"
#include "engine/render/landscape/GrassSystem.hpp"
#include "engine/render/landscape/PostFx.hpp"
#include "engine/render/landscape/ShadowMapper.hpp"
#include "engine/render/landscape/SkySystem.hpp"
#include "engine/render/landscape/TerrainLightMap.hpp"
#include "engine/render/landscape/TerrainSystem.hpp"
#include "engine/render/landscape/VegetationSystem.hpp"
#include "engine/render/landscape/WaterSystem.hpp"
#include "engine/rhi/Rhi.hpp"
#include "game/SceneSubmit.hpp" // RenderSnapshot
#include "game/scenes/AtmosphereParams.hpp"

namespace core {
class JobSystem;
}
namespace engine {
struct FrameContext;
}
namespace rhi {
class Device;
}
namespace ui {
class UiSystem;
}
namespace data {
struct LandscapeTuningForm;
}

namespace game {

class MeshCache;
class TextureCache;

// Per-frame view: everything the SIM side decides, passed by value/pointer —
// the renderer never reads the scene (audit U4-2c, the Phase-5 seam's GPU
// half). Snapshot + view in, frames out.
struct RenderView {
    render::Camera3D camera {};   // the frame's viewpoint (POD copy)
    AtmosphereParams atmos {};    // weather/panel-driven render state
    bool interiorMode { false };
    f32 timeSeconds { 0.0f };
    f32 windTime { 0.0f };
    // Moddable tuning scalars the scene owns (LandscapeTuningForm):
    f32 snowLine { 110.0f };
    f32 splatUvScale { 0.25f };
    Vec3 interiorAmbient { 0.16f, 0.15f, 0.14f };
    // 7.8ter: the player's feet part the grass (Play mode only).
    bool grassBend { false };
    Vec3 playerFeet { 0.0f };
    // Residency caches (scene-owned — the editor and streaming share them).
    MeshCache* meshCache { nullptr };
    TextureCache* materialTextures { nullptr };
    // Game UI, composed inside the backbuffer pass (null = not created).
    ::ui::UiSystem* gameUi { nullptr };
    // Stutter-hunt probe (scene-owned; render blocks report into it).
    core::FrameProbe* probe { nullptr };
};

// The custom 3D landscape renderer, extracted from LandscapeScene (audit
// U4-2c/U4-4/U4-6): owns the shader library, the render::* systems, every
// GPU handle and the frame graph (shadow cascades, reflection, main pass,
// water composite, post FX, tonemap). Consumes ONLY the RenderSnapshot and
// the RenderView. The sim side reaches the world ground truth through
// terrainParams() (terrain shape doubles as collision/nav input) and the
// panels through drawTerrainPanel()/drawRenderPanel().
class LandscapeRenderer {
public:
    // GPU resources + systems. Call after terrainParams()/applyTuning are
    // seeded (bootstrap order unchanged from the scene's onEnter).
    void create(rhi::Device& device, core::JobSystem& jobs);
    void destroy(rhi::Device& device);

    // Startup values for the render knobs the panel adjusts live (§5:
    // the TOML sets where everything starts).
    void applyTuning(const data::LandscapeTuningForm& tuning,
                     const sptr<const render::HeightPatches>& patches);

    // The whole frame: pipeline refresh, ring streaming, cascades,
    // reflection, opaque+sky+effects, copy/Hi-Z/water, post FX, tonemap
    // composite (game UI included), all from the packet + the view.
    void render(engine::FrameContext& frame, const RenderSnapshot& snapshot,
                const RenderView& view);

    // Dev panels (ImGui) — the renderer's own debug/tuning state.
    void drawTerrainPanel();                       // stats, seed, occlusion
    void drawRenderPanel(AtmosphereParams& atmos); // toggles + post sliders

    // --- Sim-side access -------------------------------------------------
    // Terrain shape = the world's ground truth (collision, nav, snaps,
    // spawn grounding all read it; the sculpt tool writes patches).
    render::TerrainParams& terrainParams() { return terrain.params; }
    const render::TerrainParams& terrainParams() const {
        return terrain.params;
    }
    render::ShaderLibrary& shaderLibrary() { return *shaders; }
    render::SkySystem& skySystem() { return sky; }
    render::TerrainSystem& terrainSystem() { return terrain; }
    render::VegetationSystem& vegetationSystem() { return vegetation; }
    render::WaterSystem& waterSystem() { return water; }
    void requestRegenerate() { regenerateRequested = true; }
    void invalidateOcclusion() { occlusion.invalidate(); }
    // Terrain sculpt: chunks awaiting a targeted GPU rebuild at the safe
    // point in render() (remesh live during the stroke; re-scatter on
    // commit only — re-seeding every preview frame would flicker).
    vector<u64>& sculptRemeshQueue() { return sculptDirtyChunks; }
    vector<u64>& sculptScatterQueue() { return sculptScatterChunks; }

    // Chantier 2 B5: the lights-UBO capacity (the extract collects this
    // many nearest LightSource entities into the snapshot).
    static constexpr u32 kMaxLights = 16;

private:
    // Offscreen color+depth target at window size, recreated on resize.
    void ensureOffscreenTarget(rhi::Device& device, u32 width, u32 height);
    void destroyOffscreenTarget(rhi::Device& device);
    void rebuildBlitPipeline(rhi::Device& device);
    void buildMeshPipeline(rhi::Device& device);
    void buildSkinnedPipeline(rhi::Device& device);
    void buildCasterPipelines(rhi::Device& device);
    void buildShaftPipeline(rhi::Device& device);
    void drawSceneMeshes(engine::FrameContext& frame,
                         const RenderSnapshot& snapshot,
                         const RenderView& view);
    void drawSkinned(engine::FrameContext& frame,
                     const RenderSnapshot& snapshot);
    void drawWaterVolumes(engine::FrameContext& frame,
                          const RenderSnapshot& snapshot);
    void drawLightShafts(engine::FrameContext& frame,
                         const RenderSnapshot& snapshot,
                         const RenderView& view, const Vec3& sunColor);
    void drawShadowCasters(engine::FrameContext& frame,
                           const RenderSnapshot& snapshot,
                           const RenderView& view, u32 cascade);
    void drawCastersInto(engine::FrameContext& frame,
                         const RenderSnapshot& snapshot,
                         const RenderView& view,
                         rhi::BindGroupHandle casterGroup, bool refreshUbos);
    // Brick 32: the water surface the camera sits under (submersion input).
    f32 effectiveWaterSurfaceY(const RenderSnapshot& snapshot,
                               const RenderView& view) const;

    uptr<render::ShaderLibrary> shaders;
    render::TerrainSystem terrain;
    render::GrassSystem grass;
    render::VegetationSystem vegetation;
    render::SkySystem sky;
    render::ChunkOcclusion occlusion;
    bool occlusionUi { true }; // height-horizon occlusion culling (A/B)
    render::GpuOcclusion gpuOcclusion;
    bool gpuOcclusionUi { true }; // Hi-Z compute culling (A/B)
    std::unordered_set<u64> gpuOccluded;      // last frame's GPU verdict
    std::unordered_set<u64> combinedOccluded; // CPU horizon ∪ GPU Hi-Z
    vector<render::TerrainSystem::ChunkAabb> occlusionAabbs;
    vector<render::GpuOcclusion::Candidate> occlusionCandidates;
    render::ShadowMapper shadows;
    render::WaterSystem water;
    render::PostFx postFx;
    // Brick 33b/c: worker-baked terrain sun-shadow + sky-openness map.
    render::TerrainLightMap terrainLightMap;
    bool terrainLightUi { true };
    bool regenerateRequested { false };
    bool wireframeUi { false };
    bool tonemapUi { true };
    bool stylizedUi { true }; // BotW step lighting vs classic wrap (A/B)
    bool shadowsUi { true };
    bool cascadeDebugUi { false };
    bool reflectionsUi { true };
    bool shaftsUi { true };
    bool contactShadowsUi { true }; // brick 33a
    bool keyShadowUi { true };      // B2b (interiors)
    bool meshShadowCastersUi { true };
    // The hysteresis-quantized sun the shadow cascades follow (a
    // continuously rotating light re-bases the texel snap every frame —
    // crawling edges); lighting keeps the smooth skyState sun.
    Vec3 shadowSunDirection { 0.0f, 1.0f, 0.0f };
    // Chantier 6 B3 (brick 28): analytical grade — OFF by default (A/B).
    bool gradingUi { false };
    f32 gradeVibranceUi { 0.3f };
    f32 gradeSplitToneUi { 0.35f };
    f32 gradeContrastUi { 1.06f };
    // Chantier 6 B4 (brick 29): auto-exposure — OFF by default (A/B).
    bool autoExposureUi { false };
    f32 autoExposureMinUi { 0.4f };
    f32 autoExposureMaxUi { 2.5f };
    f32 exposureUi { 1.0f };
    f32 ssaoUi { 0.7f };
    i32 debugBufferUi { 0 }; // 0 off, 1 bloom, 2 god rays, 3 vol, 4 ssao

    rhi::BufferHandle frameUbo {};
    rhi::BindGroupHandle frameBindGroup {};
    // Chantier 2 B5: local lights UBO (binding 5, same group as FrameUbo).
    rhi::BufferHandle lightsUbo {};

    // Chunks a sculpt changed, awaiting the safe-point rebuild in render().
    vector<u64> sculptDirtyChunks;
    vector<u64> sculptScatterChunks;

    rhi::TextureHandle whiteTexture {}; // albedoTexture = 0 -> plain tint
    rhi::SamplerHandle meshSampler {};
    // Per-snapshot-entry GPU state (tiny N; instancing per model+material
    // is the planned next step of the contract — HORIZONTAL-PASS note).
    struct MeshDraw {
        rhi::BufferHandle ubo {};
        rhi::BindGroupHandle group {};
        rhi::TextureHandle boundTexture {};
        core::Guid material {};
        rhi::BindGroupHandle casterGroup {}; // B2a: ubo at binding 4
    };
    vector<MeshDraw> meshDraws;
    rhi::PipelineHandle meshPipeline {};
    u64 meshShaderGeneration { 0 };

    // U4-2b: per-NPC draw state, keyed by entity id, mark/swept against
    // snapshot.skinned (the lightShafts pattern).
    struct SkinnedDraw {
        u64 entityId { 0 };
        bool seen { false };
        rhi::BufferHandle paletteSsbo {};
        rhi::BufferHandle modelUbo {};
        rhi::BindGroupHandle group {};
        rhi::BindGroupHandle casterGroup {}; // B2a: ubo b4 + palette b2
    };
    vector<SkinnedDraw> skinnedDraws;
    rhi::PipelineHandle skinnedPipeline {};
    u64 skinnedShaderGeneration { 0 };

    // Brick 34 (chantier 7.1): dust light shafts — one small additive
    // blade-prism per shaft light, rebuilt when its direction moves.
    struct LightShaft {
        u64 entityId { 0 };
        bool seen { false }; // mark/sweep against unloaded cells
        rhi::BufferHandle vertices {};
        rhi::BufferHandle ubo {};
        rhi::BindGroupHandle group {};
        Vec3 cachedDir { 0.0f };
        u32 vertexCount { 0 };
    };
    vector<LightShaft> lightShafts;
    rhi::PipelineHandle shaftPipeline {};
    u64 shaftShaderGeneration { 0 };
    // Brick 32 (chantier 7.4): placed water volumes — one alpha-blended
    // surface quad per volume.
    struct WaterQuad {
        u64 entityId { 0 };
        bool seen { false };
        rhi::BufferHandle vertices {};
        rhi::BufferHandle ubo {};
        rhi::BindGroupHandle group {};
    };
    vector<WaterQuad> waterQuads;
    rhi::PipelineHandle waterVolumePipeline {};
    u64 waterVolumeShaderGeneration { 0 };
    // Brick 30 (chantier 7.6): horizon cumulonimbus — a static buffer of
    // 8 camera-anchored towers, visible only while stormFront > 0.
    rhi::BufferHandle stormVertices {};
    rhi::PipelineHandle stormPipeline {};
    u64 stormShaderGeneration { 0 };
    // Brick 31 (chantier 7.7): procedural rain streaks + the top-down
    // occlusion depth (no rain under roofs) + global wetness.
    rhi::PipelineHandle rainPipeline {};
    u64 rainShaderGeneration { 0 };
    rhi::TextureHandle rainOcclusionTex {};
    rhi::FramebufferHandle rainOcclusionFb {};
    rhi::SamplerHandle rainSampler {};
    rhi::BufferHandle rainOcclusionUbo {};
    rhi::BindGroupHandle rainCasterGroup {};
    rhi::BindGroupHandle rainReceiverGroup {};

    // Chantier 6 B2a: meshes + skinned NPCs cast into the sun cascades
    // (depth-only pipelines; the model UBOs are re-used, one frame behind
    // for NPCs — invisible at shadow resolution).
    rhi::PipelineHandle meshCasterPipeline {};
    rhi::PipelineHandle skinnedCasterPipeline {};
    u64 meshCasterShaderGeneration { 0 };
    u64 skinnedCasterShaderGeneration { 0 };
    // B2b (chantier 7.5): the interior key-light shadow — ONE perspective
    // depth layer from the castsShadow light nearest the camera.
    rhi::TextureHandle keyShadowTex {};
    rhi::FramebufferHandle keyShadowFb {};
    rhi::SamplerHandle keyShadowSampler {};
    rhi::BufferHandle keyShadowUbo {};
    rhi::BindGroupHandle keyShadowCasterGroup {};
    rhi::BindGroupHandle keyShadowReceiverGroup {};

    rhi::TextureHandle offscreenColor {};
    rhi::TextureHandle offscreenDepth {};
    rhi::FramebufferHandle offscreenFb {};
    // Pre-water snapshots of the opaque scene (copyTexture targets).
    rhi::TextureHandle sceneColorCopy {};
    rhi::TextureHandle sceneDepthCopy {};
    rhi::BindGroupHandle waterSceneBindGroup {};
    // Half-res mirrored scene for the water's planar reflection.
    rhi::TextureHandle reflectionColor {};
    rhi::TextureHandle reflectionDepth {};
    rhi::FramebufferHandle reflectionFb {};
    rhi::BufferHandle reflectionUbo {};
    rhi::BindGroupHandle reflectionBindGroup {};
    rhi::SamplerHandle depthSampler {}; // nearest — depth must not filter
    rhi::SamplerHandle blitSampler {};
    // B4: one blit group per adaptation ping-pong side (binding 5 = the
    // exposure texture the tonemap taps); [0] doubles as the only group
    // on the no-postFx fallback path.
    array<rhi::BindGroupHandle, 2> blitBindGroups {};
    rhi::PipelineHandle blitPipeline {};
    u64 blitShaderGeneration { 0 };
    u32 offscreenWidth { 0 };
    u32 offscreenHeight { 0 };
};

} // namespace game
