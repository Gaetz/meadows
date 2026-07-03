#pragma once

#include "engine/render/FlyCamera.hpp"
#include "engine/rhi/Rhi.hpp"
#include "game/Scene.hpp"

namespace engine {
class Engine;
}

namespace game {

// The 3D landscape renderer prototype (custom-renderer path, Phases 11-14).
// Owns the frame: records its own render passes instead of the sprite path.
// Brick 3: fly camera (RMB mouselook + WASD/EQ) over an instanced cube grid,
// with the FrameUniforms UBO every landscape shader will share.
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
    engine::Engine* engine { nullptr };

    render::FlyCamera flyCamera;
    f32 timeSeconds { 0.0f };

    rhi::BufferHandle vertexBuffer {};
    rhi::BufferHandle indexBuffer {};
    rhi::BufferHandle instanceBuffer {};
    rhi::BufferHandle frameUbo {};
    rhi::BindGroupHandle frameBindGroup {};
    rhi::ShaderHandle shader {};
    rhi::PipelineHandle pipeline {};
    u32 instanceCount { 0 };
};

} // namespace game
