#pragma once

#include "engine/rhi/Rhi.hpp"
#include "game/Scene.hpp"

namespace engine {
class Engine;
}

namespace game {

// The 3D landscape renderer prototype (custom-renderer path, Phases 11-14).
// Owns the frame: records its own render passes instead of the sprite path.
// Brick 2: two hard-coded rotating cubes proving RHI depth test + backface
// culling. Shaders stay embedded until the ShaderLibrary brick.
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

    rhi::BufferHandle vertexBuffer {};
    rhi::BufferHandle indexBuffer {};
    rhi::BufferHandle cubeUbo[2] {};
    rhi::BindGroupHandle cubeBindGroup[2] {};
    rhi::ShaderHandle shader {};
    rhi::PipelineHandle pipeline {};

    f32 angle { 0.0f };
};

} // namespace game
