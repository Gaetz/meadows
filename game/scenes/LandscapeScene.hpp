#pragma once

#include "data/forms/FormTypeRegistry.hpp"
#include "engine/render/FlyCamera.hpp"
#include "engine/render/ShaderLibrary.hpp"
#include "game/scenes/LandscapeTuning.hpp"
#include "engine/render/landscape/ChunkOcclusion.hpp"
#include "engine/render/landscape/GpuOcclusion.hpp"
#include "engine/render/landscape/GrassSystem.hpp"
#include "engine/render/landscape/PostFx.hpp"
#include "engine/render/landscape/ShadowMapper.hpp"
#include "engine/render/landscape/SkySystem.hpp"
#include "engine/render/landscape/TerrainSystem.hpp"
#include "engine/render/landscape/VegetationSystem.hpp"
#include "engine/render/landscape/WaterSystem.hpp"
#include "engine/rhi/Rhi.hpp"
#include "game/Scene.hpp"

namespace engine {
class Engine;
}

namespace game {

// The 3D landscape renderer prototype (custom-renderer path, Phases 11-14).
// Owns the frame: records its own render passes instead of the sprite path.
// Brick 21: bloom (soft-threshold HDR pyramid, additive upsample) and
// screen-space god rays (radial march toward the sun over sky-only
// radiance), both composed in linear HDR by the tonemap pass.
class LandscapeScene final : public Scene {
public:
    explicit LandscapeScene(engine::Engine& engineContext)
        : engine(&engineContext) {}

    void onEnter() override;
    void onExit() override;

    void update(f32 dt) override;

    bool ownsFrame() const override { return true; }
    void render(engine::FrameContext& frame) override;
    void drawUi() override;

private:
    // Offscreen color+depth target at window size, recreated on resize.
    void ensureOffscreenTarget(rhi::Device& device, u32 width, u32 height);
    void destroyOffscreenTarget(rhi::Device& device);
    void rebuildBlitPipeline(rhi::Device& device);

    engine::Engine* engine { nullptr };

    // Moddable startup values (§5): loaded from data/base/landscape.toml
    // (plus any mod patches) in onEnter, then copied into the systems' plain
    // params and the UI members below — the panel still adjusts everything
    // live; the TOML sets where it all starts.
    data::FormTypeRegistry formTypes;
    LandscapeTuningForm tuning;

    // Weather (brick 24): precreated states from landscape.toml, crossfaded
    // over ~weatherDuration seconds. The blend writes into the same UI
    // members the sliders edit, so the panel shows live values and manual
    // tweaking resumes once the transition lands.
    vector<WeatherForm> weathers;
    i32 weatherSelected { -1 };  // index into weathers, -1 = manual
    WeatherForm weatherFrom;     // captured state at transition start
    f32 weatherBlend { 1.0f };   // 1 = arrived
    f32 weatherDuration { 30.0f };
    WeatherForm captureCurrentWeather() const;
    void applyWeather(const WeatherForm& w);

    render::FlyCamera flyCamera;
    f32 timeSeconds { 0.0f };

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
    bool regenerateRequested { false };
    bool wireframeUi { false };
    bool animateTime { false };
    bool tonemapUi { true };
    bool stylizedUi { true }; // BotW step lighting vs classic wrap (A/B)
    bool leafCardsUi { true }; // tree leaf-card pass (perf A/B)
    bool shadowsUi { true };
    bool cascadeDebugUi { false };
    bool reflectionsUi { true };
    f32 exposureUi { 1.0f };
    f32 cloudCoverageUi { 0.38f };
    f32 cloudShadowUi { 0.7f };
    f32 bloomIntensityUi { 0.35f };
    f32 godRayIntensityUi { 0.6f };
    f32 volumetricUi { 1.0f };
    f32 ssaoUi { 0.7f };
    i32 debugBufferUi { 0 }; // 0 off, 1 bloom, 2 god rays, 3 vol, 4 ssao
    f32 fogDensityUi { 0.0014f };
    f32 fogHeightFalloffUi { 0.02f };
    f32 fogLowBoostUi { 1.6f };
    f32 fogStartUi { 300.0f };
    f32 cloudHeightUi { 520.0f };
    f32 cloudScaleUi { 0.0011f };
    f32 sunIntensityUi { 1.0f };
    f32 ambientIntensityUi { 1.0f };
    f32 saturationUi { 1.0f };
    f32 warmthUi { 0.0f };
    f32 windStrengthUi { 1.0f };
    f32 waveChopUi { 1.0f };
    f32 windTime { 0.0f }; // accumulated wind phase (dt x strength)

    rhi::BufferHandle frameUbo {};
    rhi::BindGroupHandle frameBindGroup {};

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
    rhi::BindGroupHandle blitBindGroup {};
    rhi::PipelineHandle blitPipeline {};
    u64 blitShaderGeneration { 0 };
    u32 offscreenWidth { 0 };
    u32 offscreenHeight { 0 };
};

} // namespace game
