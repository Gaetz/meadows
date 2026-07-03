#pragma once

#include "engine/render/FlyCamera.hpp"
#include "engine/render/ShaderLibrary.hpp"
#include "engine/render/landscape/SkySystem.hpp"
#include "engine/render/landscape/TerrainSystem.hpp"
#include "engine/rhi/Rhi.hpp"
#include "game/Scene.hpp"

namespace engine {
class Engine;
}

namespace game {

// The 3D landscape renderer prototype (custom-renderer path, Phases 11-14).
// Owns the frame: records its own render passes instead of the sprite path.
// Brick 10: RHI offscreen validation — the scene renders into an offscreen
// color+depth framebuffer and reaches the backbuffer through an identity
// fullscreen blit (sampler object + framebuffer + new formats exercised).
// The image must be pixel-identical with the toggle on or off.
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

    render::FlyCamera flyCamera;
    f32 timeSeconds { 0.0f };

    uptr<render::ShaderLibrary> shaders;
    render::TerrainSystem terrain;
    render::SkySystem sky;
    bool regenerateRequested { false };
    bool wireframeUi { false };
    bool animateTime { false };
    bool offscreenUi { true };

    rhi::BufferHandle frameUbo {};
    rhi::BindGroupHandle frameBindGroup {};

    rhi::TextureHandle offscreenColor {};
    rhi::TextureHandle offscreenDepth {};
    rhi::FramebufferHandle offscreenFb {};
    rhi::SamplerHandle blitSampler {};
    rhi::BindGroupHandle blitBindGroup {};
    rhi::PipelineHandle blitPipeline {};
    u64 blitShaderGeneration { 0 };
    u32 offscreenWidth { 0 };
    u32 offscreenHeight { 0 };
};

} // namespace game
