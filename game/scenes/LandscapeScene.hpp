#pragma once

#include "data/forms/FormDatabase.hpp"
#include "data/forms/FormTypeRegistry.hpp"
#include "engine/anim/Anim.hpp"
#include "engine/assets/AssetDatabase.hpp"
#include "engine/assets/GltfMesh.hpp"
#include "engine/ecs/World.hpp"
#include "engine/render/FlyCamera.hpp"
#include "engine/render/ShaderLibrary.hpp"
#include "engine/physics/Physics.hpp"
#include "game/MeshCache.hpp"
#include "gameplay/ability/DerivedStats.hpp"
#include "gameplay/ability/GameplayEffects.hpp"
#include "gameplay/ability/GameplayTags.hpp"
#include "gameplay/stats/StatsTuning.hpp"
#include "game/SceneSubmit.hpp"
#include "game/TerrainCollision.hpp"
#include "game/TextureCache.hpp"
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
    // Themed panel sections (drawUi wraps them in collapsing headers).
    void drawGameplayUi();
    void drawSkyUi();
    void drawRenderUi();

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

    // Panel layout: themed collapsing sections, toggled by click or F-key
    // (F1-F4 via ImGui's own key state — no platform::Key extension);
    // F10 hides the whole panel (screenshots, immersion).
    bool uiPanelVisible { true };
    bool uiGameplayOpen { true };
    bool uiTerrainOpen { false };
    bool uiSkyOpen { false };
    bool uiRenderOpen { false };

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

    // B1 (chantier 1): the real mesh path replacing the H8 hardcoded cube.
    // A small ECS world spawned from plugin ReferenceForms; extractMeshes
    // fills the snapshot each frame; the residency caches resolve guids to
    // GPU resources (placeholders while pending — never block, §7).
    data::FormDatabase forms;      // resolved plugin stack (member: material
                                   //   lookups happen at draw time)
    assets::AssetDatabase assetDb; // guid -> file, layered per plugin order
    ecs::World world;
    uptr<TextureCache> materialTextures; // SRGBA8 + Linear (3D albedo)
    uptr<MeshCache> meshCache;
    RenderSnapshot snapshot;
    rhi::TextureHandle whiteTexture {}; // albedoTexture = 0 -> plain tint
    rhi::SamplerHandle meshSampler {};
    // Per-snapshot-entry GPU state (tiny N; instancing per model+material
    // is the planned next step of the contract — HORIZONTAL-PASS note).
    struct MeshDraw {
        rhi::BufferHandle ubo {};
        rhi::BindGroupHandle group {};
        rhi::TextureHandle boundTexture {};
        core::Guid material {};
    };
    vector<MeshDraw> meshDraws;
    rhi::PipelineHandle meshPipeline {};
    u64 meshShaderGeneration { 0 };
    void buildMeshPipeline(rhi::Device& device);
    void drawSceneMeshes(engine::FrameContext& frame);

    // B6 (chantier 1): Forms-driven skinned NPCs. The scene builds NOTHING
    // by hand anymore — every actor whose ActorForm resolves an
    // ActorVisual (appearance + animGraph) gets a GPU skin, a data-built
    // locomotion graph, and a patrol brain. One rig cache per glTF asset
    // (sync load at enter; the async path joins the caches in chantier 2).
    struct RigData {
        anim::Skeleton skeleton;
        vector<assets::GltfClip> clips;
    };
    std::unordered_map<core::Guid, RigData> rigCache;
    const RigData* loadRig(const core::Guid& asset);
    // Per-NPC runtime state (non-reflected, §H5). uptr: the GraphInstance
    // references Npc::graph — addresses must survive vector growth.
    struct Npc {
        ecs::Entity entity;
        const RigData* rig { nullptr };
        anim::GraphDesc graph; // owns the clips; `anim` references it
        uptr<anim::GraphInstance> anim;
        anim::Pose pose;
        vector<Mat4> palette;
        Vec4 tint { 1.0f };
        rhi::BufferHandle vertices {};
        rhi::BufferHandle indices {};
        u32 indexCount { 0 };
        rhi::BufferHandle paletteSsbo {};
        rhi::BufferHandle modelUbo {};
        rhi::BindGroupHandle group {};
        // Patrol: walk to patrolPoints[target], pause, swap ends.
        u32 target { 0 };
        f32 pauseTimer { 0.0f };
        f32 yaw { 0.0f };
        f32 speed { 0.0f }; // smoothed horizontal speed -> anim param
    };
    vector<uptr<Npc>> npcs;
    vector<Vec3> patrolPoints;   // grounded "patrol" marker positions
    Vec3 characterSpot { 0.0f }; // first NPC position (teleport target)
    rhi::PipelineHandle skinnedPipeline {};
    u64 skinnedShaderGeneration { 0 };
    void buildSkinnedPipeline(rhi::Device& device);
    void setupNpcs(rhi::Device& device);
    void updateNpcs(f32 dt);
    void drawNpcs(engine::FrameContext& frame);

    // B4 (chantier 1): physics — height-field tiles follow the camera (the
    // player takes over as focus in B5); the debug capsule proves the
    // fall/rest/slope behavior in-scene (drawn as the placeholder box).
    uptr<phys::PhysicsWorld> physics;
    uptr<TerrainCollision> terrainCollision;
    uptr<phys::CharacterBody> debugCapsule;

    // B5: first-person Play mode (the game IS first-person — acted
    // decision). The player is a kinematic capsule, the camera sits at eye
    // height, the mouse is always captured; Fly stays the dev camera.
    // Toggle: F key or the checkbox.
    bool playMode { false };
    uptr<phys::CharacterBody> player;
    Vec3 playerVelocity { 0.0f }; // smoothed horizontal velocity (m/s)
    f32 jumpSpeed { 5.0f };       // jump power stat = the P1 stats pass
    void enterPlayMode();
    void exitPlayMode();
    void updatePlayer(f32 dt);

    // B5.5: the player is a GAS actor (docs/STATS.md) — spawned from the
    // "Player" ActorForm, ticked by tickCharacter; the controller READS
    // the derived movementSpeed/acceleration currents and pays sprint
    // through the SprintCost GameplayEffect (§2.9: never set directly).
    gameplay::DerivedStatRegistry derivedStats;
    gameplay::GameplayTagRegistry gameTags;
    gameplay::StatsTuningForm statsTuning;
    ecs::Entity playerEntity {};
    f32 sprintCostAccumulator { 0.0f };
    const gameplay::EffectForm* sprintCostEffect { nullptr };
    const gameplay::EffectForm* testWoundEffect { nullptr };

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
