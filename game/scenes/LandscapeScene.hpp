#pragma once

#include "engine/render/FlyCamera.hpp"
#include "engine/render/ShaderLibrary.hpp"
#include "engine/render/landscape/TerrainSystem.hpp"
#include "engine/rhi/Rhi.hpp"
#include "game/Scene.hpp"

namespace engine {
class Engine;
}

namespace game {

// The 3D landscape renderer prototype (custom-renderer path, Phases 11-14).
// Owns the frame: records its own render passes instead of the sprite path.
// Brick 8: LOD rings + skirts on the streamed terrain — mesh density falls
// with distance (65/33/17/9 verts per side), skirts cover LOD seams, and the
// view radius grows to ~900 m. Wireframe toggle to inspect the rings.
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

    uptr<render::ShaderLibrary> shaders;
    render::TerrainSystem terrain;
    bool regenerateRequested { false };
    bool wireframeUi { false };

    rhi::BufferHandle frameUbo {};
    rhi::BindGroupHandle frameBindGroup {};
};

} // namespace game
