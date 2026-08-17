#pragma once

#include <unordered_set>

#include "engine/core/FrameProbe.hpp"
#include "engine/render/Camera3D.hpp"
#include "engine/render/Frustum.hpp"
#include "engine/render/GpuProbe.hpp"
#include "engine/render/ShaderLibrary.hpp"
#include "engine/render/landscape/ChunkOcclusion.hpp"
#include "engine/render/landscape/GpuOcclusion.hpp"
#include "engine/render/landscape/GrassSystem.hpp"
#include "engine/render/landscape/LightClusters.hpp"
#include "engine/render/landscape/PostFx.hpp"
#include "engine/render/landscape/RadianceCascades.hpp"
#include "engine/render/landscape/ShadowMapper.hpp"
#include "engine/render/landscape/SkySystem.hpp"
#include "engine/render/landscape/FarTerrain.hpp"
#include "engine/render/landscape/MistMap.hpp"
#include "engine/render/landscape/NoiseVolume.hpp"
#include "engine/render/landscape/TerrainLightMap.hpp"
#include "engine/render/landscape/TerrainShadeMap.hpp"
#include "engine/render/landscape/TerrainSystem.hpp"
#include "engine/render/landscape/VegetationSystem.hpp"
#include "engine/render/landscape/FxRenderer.hpp"
#include "engine/render/landscape/WaterSystem.hpp"
#include "engine/rhi/Rhi.hpp"
#include "engine/rhi/UniqueHandle.hpp"
#include "engine/render/SceneView.hpp" // render::RenderSnapshot
#include "engine/render/AtmosphereParams.hpp"

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
// Game-side friends: the ImGui tuning panels and the Forms<->flat-params
// tuning seam operate on the renderer's private knobs (names only — the
// engine includes nothing from game/).
namespace game {
class RenderTuningPanels;
class RenderTuningIo;
}

namespace render {

class MeshCache;
class TextureCache;

// Per-subsystem opt-in (docs/RENDERING.md §7): a tool scene mounts only
// what it needs — a system left off is never created, allocated or
// ticked (its GPU resources don't exist; render() skips its passes).
// Defaults = the full game renderer, so LandscapeScene passes nothing.
// The always-on core is meshes + skinned NPCs + CSM/key shadows +
// lights + tonemap composite. Terrain includes its ring streaming and
// the terrain light map; sky includes weather (cloud bake, rain/storm).
struct RendererConfig {
    bool terrain { true };
    bool water { true };      // sea plane, planar reflection, volumes
    bool sky { true };        // dome, cloud bake, rain occlusion/streaks
    bool vegetation { true };
    bool grass { true };
    bool gi { true };         // radiance cascades
    bool froxels { true };    // froxel fog (needs postFx)
    bool occlusion { true };  // CPU horizon + GPU Hi-Z
    bool postFx { true };     // bloom/rays/volumetric/contact/auto-expo
    // Cooked terrain material arrays (.mtex file paths, resolved by the
    // scene from the plugin VFS — the renderer never sees a Form). Empty =
    // procedural splat tiles; ignored without caps.textureCompressionBC.
    str terrainAlbedoPath;
    str terrainNormalPath;
    str terrainOrmPath;
    str terrainHeightPath;
};

// Per-frame view: everything the SIM side decides, passed by value/pointer —
// the renderer never reads the scene (the Phase-5 seam's GPU
// half). Snapshot + view in, frames out.
struct RenderView {
    render::Camera3D camera {};   // the frame's viewpoint (POD copy)
    AtmosphereParams atmos {};    // weather/panel-driven render state
    bool interiorMode { false };
    f32 timeSeconds { 0.0f };
    f32 windTime { 0.0f };
    // Moddable tuning scalars the scene owns (LandscapeTuningForm):
    f32 snowLine { render::kSnowLine };
    f32 splatUvScale { 0.25f };
    f32 splatBlendDepth { 0.15f }; // height-blend band (0 = plain blend)
    f32 terrainTintStrength { 0.3f }; // macro tint (0 = off, <= ~0.4)
    f32 splatDetailFade { 24.0f }; // detail-normal fade end (m, 0 = off)
    f32 pomDistance { 12.0f }; // parallax occlusion reach (m, 0 = off)
    f32 splatVariety { 0.5f }; // anti-repetition second tap (0 = off)
    f32 pomShadowStrength { 0.6f }; // POM self-shadow (0 = off)
    f32 pomDepth { 0.03f }; // parallax relief depth (uv units)
    Vec3 interiorAmbient { 0.16f, 0.15f, 0.14f };
    // H3: the active worldspace's buried threshold (-1e9 = rule off).
    f32 buriedBelowY { -1.0e9f };
    // The player's feet part the grass (Play mode only).
    bool grassBend { false };
    Vec3 playerFeet { 0.0f };
    // Residency caches (scene-owned — the editor and streaming share them).
    render::MeshCache* meshCache { nullptr };
    render::TextureCache* materialTextures { nullptr };
    // Game UI, composed inside the backbuffer pass (null = not created).
    ::ui::UiSystem* gameUi { nullptr };
    // Stutter-hunt probe (scene-owned; render blocks report into it).
    core::FrameProbe* probe { nullptr };
};

// The engine's world renderer (multi-instance, per-subsystem config —
// docs/RENDERING.md §7): owns the shader library, the render::* systems,
// every GPU handle and the frame graph (shadow cascades, reflection, main
// pass, water composite, post FX, tonemap). Consumes ONLY the
// RenderSnapshot (SceneView.hpp) and the RenderView. The sim side reaches
// the world ground truth through terrainParams() (terrain shape doubles
// as collision/nav input). The dev tuning/perf panels live in
// game/ui/RenderTuningPanels, and the Forms<->flat-params startup seam in
// game/scenes/RenderTuningIo — both friends editing the live knobs in
// place (the engine never sees a Form, CLAUDE.md §4).
class WorldRenderer {
public:
    // GPU resources + systems. Call after terrainParams()/the tuning
    // seam are seeded (bootstrap order unchanged from the scene's
    // onEnter). `config` selects the mounted subsystems (default:
    // everything).
    void create(rhi::Device& device, core::JobSystem& jobs,
                const RendererConfig& config = {});
    void destroy(rhi::Device& device);

    const RendererConfig& config() const { return cfg; }

    // True once when a panel's Save button was pressed (the scene owns
    // the plugin stack and performs the write).
    bool consumeSaveTuningRequest() {
        const bool requested = saveTuningRequested;
        saveTuningRequested = false;
        return requested;
    }

    // The whole frame: pipeline refresh, ring streaming, cascades,
    // reflection, opaque+sky+effects, copy/Hi-Z/water, post FX, tonemap
    // composite (game UI included), all from the packet + the view.
    void render(engine::FrameContext& frame,
                const render::RenderSnapshot& snapshot,
                const RenderView& view);

    // --- Sim-side access -------------------------------------------------
    // Terrain shape = the world's ground truth (collision, nav, snaps,
    // spawn grounding all read it; the sculpt tool writes patches).
    render::TerrainParams& terrainParams() { return terrain.params; }

    // Scanned-prop overrides (scene wiring, docs/GRASS-REDO.md): replaces
    // one vegetation variant's mesh after create() — the scene resolves
    // the glTF through the VFS, decimates and normalizes it first.
    void overrideVegetationMesh(rhi::Device& device, u32 variant,
                                render::MeshData mesh,
                                render::MeshData low = {},
                                render::MeshData ultra = {}) {
        vegetation.overrideVariantMesh(device, variant, std::move(mesh),
                                       std::move(low), std::move(ultra));
    }
    void setVegetationBark(rhi::Device& device,
                           render::VegetationSystem::BarkImage oakAlbedo,
                           render::VegetationSystem::BarkImage oakNrm,
                           render::VegetationSystem::BarkImage pineAlbedo,
                           render::VegetationSystem::BarkImage pineNrm) {
        vegetation.setBarkTextures(device, std::move(oakAlbedo),
                                   std::move(oakNrm),
                                   std::move(pineAlbedo),
                                   std::move(pineNrm));
    }
    void overrideVegetationAlbedo(rhi::Device& device, u32 variant,
                                  u32 width, u32 height, vector<u8> rgba,
                                  u32 normalWidth = 0,
                                  u32 normalHeight = 0,
                                  vector<u8> normalRgba = {}) {
        vegetation.setVariantAlbedo(device, variant, width, height,
                                    std::move(rgba), normalWidth,
                                    normalHeight, std::move(normalRgba));
    }
    const render::TerrainParams& terrainParams() const {
        return terrain.params;
    }
    render::ShaderLibrary& shaderLibrary() { return *shaders; }
    render::SkySystem& skySystem() { return sky; }
    render::TerrainSystem& terrainSystem() { return terrain; }
    render::VegetationSystem& vegetationSystem() { return vegetation; }
    render::WaterSystem& waterSystem() { return water; }
    void requestRegenerate() { regenerateRequested = true; }
    // Variant meshes only (scatter/instances stay) — the tree builder's
    // regen trigger, applied at render()'s safe point.
    void requestReseedVegetation() { reseedVegetation = true; }
    void invalidateOcclusion() { occlusion.invalidate(); }
    // Terrain sculpt: chunks awaiting a targeted GPU rebuild at the safe
    // point in render() (remesh live during the stroke; re-scatter on
    // commit only — re-seeding every preview frame would flicker).
    vector<u64>& sculptRemeshQueue() { return sculptDirtyChunks; }
    vector<u64>& sculptScatterQueue() { return sculptScatterChunks; }
    // Scene-driven streaming gates (sandbox boot): `hold` parks the
    // terrain/grass/vegetation rings while the base regions are not
    // published yet (meshing against the empty base would all be redone
    // on publish); `boost` widens the anti-stutter streaming budgets
    // while an opaque loading veil hides the frame.
    void setStreamingHold(bool hold) { streamingHold = hold; }
    void setStreamingBoost(bool boost) { streamingBoost = boost; }

    // The lights-UBO capacity (the extract collects this many selected
    // LightSource entities into the snapshot). The full budget is only
    // consumable through the clustered path (docs/RENDERING.md §5); with
    // clustered off, the per-pixel loop clamps to kFallbackLights.
    static constexpr u32 kMaxLights = 64;
    static constexpr u32 kFallbackLights = 24;

    // Every live tuning knob in ONE public struct: the dev panels
    // (game/ui/RenderTuningPanels) and the Forms<->flat-params seam
    // (game/scenes/RenderTuningIo) edit it in place, render() reads it.
    // Friendship below remains for what the panels drive BEYOND knobs
    // (subsystem stats and internals, regen/save requests).
    struct RenderTuning {
        bool occlusion { true }; // height-horizon occlusion culling (A/B)
        // Seasons (until a season system drives them): autumn color blend
        // and deciduous leaf fall, per-slot weighting from the species.
        f32 seasonAutumn { 0.0f };
        f32 seasonLeafFall { 0.0f };
        bool gpuOcclusion { true }; // Hi-Z compute culling (A/B)
        // GPU-driven terrain path (docs/RENDERING.md §6.0): the cull's
        // commands drive drawIndexedIndirect — no CPU verdict on this path.
        bool gpuIndirect { true };
        // CSM round-robin (docs/RENDERING.md): cascade 0 renders every
        // frame, the far cascades alternate. A SKIPPED cascade keeps its
        // previous matrix (receiver and caster UBOs alike) so the stale
        // depth still matches; a sun step re-renders everything that frame.
        bool shadowRoundRobin { true };
        // CSM sharpness: texels per cascade side — recreate keyed on the
        // applied value (the reflectionScale pattern).
        i32 shadowResolution { 2048 };
        // The planar reflection levers: auto-skip renders it only when a
        // RESIDENT below-sea chunk is in the frustum (edge case: sea at
        // the horizon beyond the ring — the A/B toggle exists for exactly
        // that check), and the resolution scale trades mirror sharpness
        // for fill rate.
        bool reflectionAutoSkip { true };
        f32 reflectionScale { 0.5f };
        // Clustered-forward light culling (docs/RENDERING.md §5); gates
        // both the dispatch and the shaders' clustered path.
        bool clusteredLights { true };
        bool terrainLight { true }; // worker-baked terrain sun/sky map
        bool farTerrain { true };   // distant silhouettes (§3.6)
        bool mist { true };         // ground-mist raymarch (§3.5)
        // Live mist tuning (panel "Ground mist"); density/coverage are
        // weather-owned and ride AtmosphereParams instead.
        f32 mistCoverageSoftness { 0.6f };
        f32 mistReach { 1200.0f };
        Vec4 mistShape { 0.0035f, 0.02f, 0.2f, 49.0f };
        // Volumetric sky clouds (§8): A/B vs the 2D dome layer + the
        // shape and light knobs (panel "Sky clouds"; thickness/sigma/
        // erosion — coverage/height/scale stay the weather cloudInfo
        // lanes).
        bool skyClouds { true };
        // x thickness (m), y sigma (1/m), z erosion, w thickness<->coverage.
        Vec4 skyCloudShape { 440.0f, 0.065f, 0.31f, 3.4f };
        // x body gain, y body g, z ambient gain, w lining gain.
        Vec4 skyCloudLight { 19.9f, 0.3f, 0.9f, 30.2f };
        f32 skyCloudLiningLobe { 0.8f }; // lining HG g (halo tightness)
        f32 skyCloudPowder { 1.0f };     // dark-edge strength
        f32 skyCloudPuffiness { 0.5f };  // fractal edge erosion
        f32 skyCloudRimGain { 25.0f };   // view-thin silhouette glow
        f32 skyCloudRimLobe { 0.75f };
        f32 skyCloudBaseDark { 7.4f };   // storm-base ambient occlusion
        bool mistNoiseTex { true }; // A/B: baked volume vs analytic fbm3
        f32 mistDetailDropout { 400.0f };
        i32 mistSteps { 16 }; // per-pixel march steps (EMA covers the rest)
        f32 mistPuffiness { 0.5f }; // fractal edge florets
        f32 mistSunBoost { 10.0f }; // sun-beam gain (silver lining)
        // Light shaping: x = forward HG lobe g, y = backscatter weight,
        // z = ambient gain, w = ambient floor in shadow.
        Vec4 mistLight { 0.95f, 0.8f, 1.25f, 0.6f };
        bool wireframe { false };
        bool tonemap { true };
        // BotW step lighting vs classic wrap (A/B). Off: the realistic
        // terrain-texturing look owns this branch; the panel checkbox
        // brings the cel ramp back.
        bool stylized { false };
        bool shadows { true };
        bool cascadeDebug { false };
        bool reflections { true };
        bool contactShadows { true };
        // Half-res SSAO (ssao.frag): contact-scale crevice darkening —
        // the realistic-branch companion of the material relief.
        bool ssao { true };
        f32 ssaoStrength { 0.85f };
        f32 ssaoRadius { 0.7f };
        // SSDM (ssdm_*.frag — Lobel 2008): screen-space scatter of the
        // alpha-packed relief. Mode: 0 = off, 1 = half-res chain +
        // edge-aware upsample (~2.4 ms), 2 = full res (~17 ms on M1,
        // kept for its crispness).
        i32 ssdmMode { 2 };
        f32 ssdmAmp { 0.12f }; // world amplitude (m)
        bool keyShadow { true }; // interiors
        bool meshShadowCasters { true };
        // Analytical grade — OFF by default while the GI is tuned (the
        // grade masks what the light bounces are doing; revisit later);
        // the A/B checkbox remains.
        bool grading { false };
        f32 gradeVibrance { 0.3f };
        f32 gradeSplitTone { 0.35f };
        f32 gradeContrast { 1.06f };
        // Auto-exposure — ON by default; the A/B checkbox remains.
        bool autoExposure { true };
        f32 autoExposureMin { 0.4f };
        f32 autoExposureMax { 2.5f };
        // Stylized ramp (stylized.glsl lanes; defaults = shipped cel look).
        Vec4 stylizedDiffuse { 0.02f, 0.09f, 0.32f, 0.40f };
        Vec4 stylizedShadow { 0.45f, 0.55f, 0.0f, 0.6f };
        // Character/prop cel specular: strength, threshold, exponent.
        Vec4 stylizedSpec { 0.35f, 0.35f, 24.0f, 0.0f };
        f32 interiorDaylightWeight { 0.6f }; // interior<->outside coupling
        f32 interiorDustDensity { 0.025f };  // uniform dust indoors
        f32 exposure { 1.0f };
        i32 debugBuffer { 0 }; // 0 off, 1 bloom, 2 god rays, 3 volumetric
        // Water debug view (panel > Water): 0 off, 1 flow, 2 torrent,
        // 3 river UV, 4-6 water-info texture channels.
        i32 waterDebug { 0 };
    };
    RenderTuning tuning;

private:
    // Game-side dev UI and tuning seam edit the state below in place.
    friend class ::game::RenderTuningPanels;
    friend class ::game::RenderTuningIo;

    RendererConfig cfg {};

    // Offscreen color+depth target at window size, recreated on resize.
    void ensureOffscreenTarget(rhi::Device& device, u32 width, u32 height);
    void destroyOffscreenTarget(rhi::Device& device);
    void rebuildBlitPipeline(rhi::Device& device);
    void buildMeshPipeline(rhi::Device& device);
    void buildSkinnedPipeline(rhi::Device& device);
    void buildCasterPipelines(rhi::Device& device);
    void drawSceneMeshes(engine::FrameContext& frame,
                         const render::RenderSnapshot& snapshot,
                         const RenderView& view,
                         const render::Frustum* cull = nullptr);
    // The GI chain's per-frame recording — post-CSM slot, or end of
    // frame when pipelined (docs/RENDERING.md PG2).
    void recordGiUpdate(engine::FrameContext& frame,
                        const render::RenderSnapshot& snapshot,
                        const RenderView& view,
                        const render::FrameUniforms& uniforms,
                        bool clustered);
    void drawSkinned(engine::FrameContext& frame,
                     const render::RenderSnapshot& snapshot);
    void drawWaterVolumes(engine::FrameContext& frame,
                          const render::RenderSnapshot& snapshot);
    // `cull` = the target volume's frustum (cascade ortho / key tile /
    // rain window); null draws everything.
    void drawShadowCasters(engine::FrameContext& frame,
                           const render::RenderSnapshot& snapshot,
                           const RenderView& view, u32 cascade,
                           const render::Frustum* cull = nullptr);
    void drawCastersInto(engine::FrameContext& frame,
                         const render::RenderSnapshot& snapshot,
                         const RenderView& view,
                         rhi::BindGroupHandle casterGroup, bool refreshUbos,
                         const render::Frustum* cull = nullptr);
    // One selected key-shadow light: its anchor (the UBO match key —
    // the light's original position) and the tile's perspective matrix.
    struct KeyShadowPick {
        Vec3 anchor;
        Mat4 viewProj;
    };
    // render()'s pass sequence, one method per pass (pure code motion):
    // render() itself reads as the frame's table of contents and owns
    // only the per-frame decisions (cascade fit, key-light picks, the
    // uniform composition) that the passes consume.
    void pumpPipelinesAndRequests(engine::FrameContext& frame);
    void pumpStreaming(engine::FrameContext& frame, const RenderView& view);
    void recordKeyShadowTiles(engine::FrameContext& frame,
                              const render::RenderSnapshot& snapshot,
                              const RenderView& view,
                              const vector<KeyShadowPick>& keyShadowPicks);
    void recordRainOcclusion(engine::FrameContext& frame,
                             const render::RenderSnapshot& snapshot,
                             const RenderView& view,
                             const render::FrameUniforms& frameData);
    void recordShadowCascades(
        engine::FrameContext& frame,
        const render::RenderSnapshot& snapshot, const RenderView& view,
        const render::ShadowMapper::Cascades& cascades,
        const array<bool, render::ShadowMapper::kCascadeCount>& cascadeDue,
        f32 shadowStrength);
    void recordReflection(engine::FrameContext& frame,
                          const RenderView& view,
                          const render::FrameUniforms& uniforms,
                          bool reflectionsActive);
    void recordMainPass(engine::FrameContext& frame,
                        const render::RenderSnapshot& snapshot,
                        const RenderView& view,
                        const render::Frustum& viewFrustum,
                        const render::FrameUniforms& frameData,
                        bool useOffscreen);
    void recordCopyHizWater(engine::FrameContext& frame,
                            const RenderView& view, const Mat4& viewProj,
                            bool useOffscreen);
    void recordPostFx(engine::FrameContext& frame, const RenderView& view,
                      const render::FrameUniforms& frameData,
                      bool useOffscreen);
    // The water surface the camera sits under (submersion input).
    f32 effectiveWaterSurfaceY(const render::RenderSnapshot& snapshot,
                               const RenderView& view) const;

    uptr<render::ShaderLibrary> shaders;
    render::TerrainSystem terrain;
    render::GrassSystem grass;
    render::VegetationSystem vegetation;
    render::SkySystem sky;
    render::ChunkOcclusion occlusion;
    render::GpuOcclusion gpuOcclusion;
    std::unordered_set<u64> combinedOccluded; // CPU horizon (legacy path)
    vector<render::TerrainSystem::ChunkAabb> occlusionAabbs;
    vector<render::GpuOcclusion::Candidate> occlusionCandidates;
    // Set by the cull's run() each frame, consumed by the NEXT frame's
    // main pass: the indirect commands are at most one frame stale
    // (returning from an interior invalidates them).
    bool occlusionCommandsFresh { false };
    render::ShadowMapper shadows;
    f32 appliedReflectionScale { 0.5f };
    render::ShadowMapper::Cascades lastCascades {};
    bool lastCascadesValid { false };
    u64 shadowFrame { 0 };
    render::WaterSystem water;
    render::FxRenderer fx; // the CPU-particle pass
    render::PostFx postFx;
    render::LightClusters lightClusters;
    render::GpuProbe gpuProbe; // per-pass GPU budget (docs/RENDERING.md)
    u64 perfFrames { 0 }; // the one-shot "gpu budget" log's frame count
    // Worker-baked terrain sun-shadow + sky-openness map.
    render::TerrainLightMap terrainLightMap;
    render::TerrainShadeMap terrainShadeMap;
    // Distant landscape silhouettes beyond the streaming ring (§3.6).
    render::FarTerrain farTerrain;
    // Worker-baked valley data for the ground-mist raymarch (§3.5).
    render::MistMap mistMap;
    // Shared tileable Perlin-Worley volume (mist + sky-cloud erosion).
    render::NoiseVolume noiseVolume;
    // The GI voxel clipmap (docs/RENDERING.md) +
    // cascades; its tuning is the render panel's "Global illumination".
    render::RadianceCascades radianceCascades;
    vector<render::RcBox> rcBoxes;     // per-frame injection lists,
    vector<render::RcLight> rcLights;  // reused to avoid re-allocations
    vector<render::VegetationSystem::GiProp> vegGiProps; // forests -> GI
    bool regenerateRequested { false };
    // A/B checkbox flips this; render() swaps the variant meshes at its
    // safe point (mesh-only — instance buffers and scatter stay resident).
    bool reseedVegetation { false };
    // Grass panel: a scatter knob moved — grass-only re-scatter next frame.
    bool grassRescatterRequested { false };
    // The hysteresis-quantized sun the shadow cascades follow (a
    // continuously rotating light re-bases the texel snap every frame —
    // crawling edges); lighting keeps the smooth skyState sun.
    Vec3 shadowSunDirection { 0.0f, 1.0f, 0.0f };
    bool saveTuningRequested { false }; // panels' Save button -> the scene
    // Stamp of the mesh array the caster pass indexed meshDraws with;
    // the lit pass asserts it saw the same one, then clears it.
    const void* casterMeshesData { nullptr };

    rhi::UniqueBuffer frameUbo;
    rhi::UniqueBindGroup frameBindGroup;
    // Local lights UBO (binding 5, same group as FrameUbo).
    rhi::UniqueBuffer lightsUbo;

    // Chunks a sculpt changed, awaiting the safe-point rebuild in render().
    vector<u64> sculptDirtyChunks;
    vector<u64> sculptScatterChunks;
    // Scene-driven streaming gates — see setStreamingHold/Boost.
    bool streamingHold { false };
    bool streamingBoost { false };

    rhi::UniqueTexture whiteTexture; // albedoTexture = 0 -> plain tint
    rhi::UniqueSampler meshSampler;
    // Per-snapshot-entry GPU state (tiny N; instancing per model+material
    // is the planned next step of the contract — HORIZONTAL-PASS note).
    // Unique members — vector erase/clear frees the GPU state.
    struct MeshDraw {
        rhi::UniqueBuffer ubo;
        rhi::UniqueBindGroup group;
        rhi::TextureHandle boundTexture {}; // NON-owning (residency cache)
        core::Guid material {};
        rhi::UniqueBindGroup casterGroup; // ubo at binding 4
    };
    vector<MeshDraw> meshDraws;
    rhi::UniquePipeline meshPipeline;
    u64 meshShaderGeneration { 0 };

    // Per-NPC draw state, keyed by entity id, mark/swept against
    // snapshot.skinned (mark/sweep by entity id).
    struct SkinnedDraw {
        u64 entityId { 0 };
        bool seen { false };
        rhi::UniqueBuffer paletteSsbo {};
        rhi::UniqueBuffer modelUbo {};
        rhi::UniqueBindGroup group {};
        rhi::UniqueBindGroup casterGroup {}; // ubo b4 + palette b2
    };
    vector<SkinnedDraw> skinnedDraws;
    rhi::UniquePipeline skinnedPipeline;
    u64 skinnedShaderGeneration { 0 };

    // Placed water volumes — one alpha-blended
    // surface quad per volume.
    struct WaterQuad {
        u64 entityId { 0 };
        bool seen { false };
        rhi::UniqueBuffer vertices {};
        rhi::UniqueBuffer ubo {};
        rhi::UniqueBindGroup group {};
    };
    vector<WaterQuad> waterQuads;
    rhi::UniquePipeline waterVolumePipeline;
    u64 waterVolumeShaderGeneration { 0 };
    // Procedural rain streaks + the top-down
    // occlusion depth (no rain under roofs) + global wetness.
    rhi::UniquePipeline rainPipeline;
    u64 rainShaderGeneration { 0 };
    rhi::UniqueTexture rainOcclusionTex;
    rhi::UniqueFramebuffer rainOcclusionFb;
    rhi::UniqueSampler rainSampler;
    rhi::UniqueBuffer rainOcclusionUbo;
    rhi::UniqueBindGroup rainCasterGroup;
    rhi::UniqueBindGroup rainReceiverGroup;

    // Meshes + skinned NPCs cast into the sun cascades
    // (depth-only pipelines; the model UBOs are re-used, one frame behind
    // for NPCs — invisible at shadow resolution).
    rhi::UniquePipeline meshCasterPipeline;
    rhi::UniquePipeline skinnedCasterPipeline;
    u64 meshCasterShaderGeneration { 0 };
    u64 skinnedCasterShaderGeneration { 0 };
    // The interior key-light shadow — ONE perspective
    // depth layer from the castsShadow light nearest the camera.
    // Key-shadow ATLAS (docs/RENDERING.md §5 B6): one 2048^2 depth target,
    // 2x2 tiles of 1024^2 — the up-to-4 best-scored castsShadow lights
    // render one tile each (one caster UBO/group per tile: the same
    // buffer updated 4x mid-recording would clobber itself).
    static constexpr u32 kKeyShadowSlots = 4;
    rhi::UniqueTexture keyShadowTex;
    rhi::UniqueFramebuffer keyShadowFb;
    rhi::UniqueSampler keyShadowSampler;
    array<rhi::UniqueBuffer, kKeyShadowSlots> keyShadowUbos;
    array<rhi::UniqueBindGroup, kKeyShadowSlots> keyShadowCasterGroups;
    rhi::UniqueBindGroup keyShadowReceiverGroup;

    rhi::UniqueTexture offscreenColor;
    rhi::UniqueTexture offscreenDepth;
    rhi::UniqueFramebuffer offscreenFb;
    // Pre-water snapshots of the opaque scene (copyTexture targets).
    rhi::UniqueTexture sceneColorCopy;
    rhi::UniqueTexture sceneDepthCopy;
    rhi::UniqueBindGroup waterSceneBindGroup;
    // SSDM scatter chain (ssdm_*.frag — Lobel 2008): flow (per-pixel
    // displacement + displaced depth) -> bounds quadtree (5 levels) ->
    // resolve (nearest-wins, gather fallback) into the offscreen target.
    static constexpr u32 kSsdmLevels = 5;
    rhi::UniquePipeline ssdmFlowPipeline;
    rhi::UniquePipeline ssdmBounds0Pipeline;
    rhi::UniquePipeline ssdmDownPipeline;
    rhi::UniquePipeline ssdmResolvePipeline;
    rhi::UniqueTexture ssdmFlowTex;
    rhi::UniqueFramebuffer ssdmFlowFb;
    rhi::UniqueBindGroup ssdmFlowGroup;
    array<rhi::UniqueTexture, kSsdmLevels> ssdmBoundsTex;
    array<rhi::UniqueFramebuffer, kSsdmLevels> ssdmBoundsFb;
    array<rhi::UniqueBindGroup, kSsdmLevels> ssdmBoundsGroup;
    rhi::UniqueBindGroup ssdmResolveGroup;
    // Half mode (tuning.ssdmMode == 1): the resolve lands in this chain-res
    // intermediate (alpha = moved flag), then the upsample rewrites the
    // touched full-res pixels only.
    rhi::UniquePipeline ssdmResolveHalfPipeline;
    rhi::UniquePipeline ssdmUpsamplePipeline;
    rhi::UniqueTexture ssdmHalfTex;
    rhi::UniqueFramebuffer ssdmHalfFb;
    rhi::UniqueBindGroup ssdmUpsampleGroup;
    i32 appliedSsdmMode { -1 };
    // Half-res mirrored scene for the water's planar reflection.
    rhi::UniqueTexture reflectionColor;
    rhi::UniqueTexture reflectionDepth;
    rhi::UniqueFramebuffer reflectionFb;
    rhi::UniqueBuffer reflectionUbo;
    rhi::UniqueBindGroup reflectionBindGroup;
    rhi::UniqueSampler depthSampler; // nearest — depth must not filter
    rhi::UniqueSampler blitSampler;
    // One blit group per adaptation ping-pong side (binding 5 = the
    // exposure texture the tonemap taps); [0] doubles as the only group
    // on the no-postFx fallback path.
    array<rhi::UniqueBindGroup, 2> blitBindGroups;
    rhi::UniquePipeline blitPipeline;
    u64 blitShaderGeneration { 0 };
    u32 offscreenWidth { 0 };
    u32 offscreenHeight { 0 };
};

} // namespace render
