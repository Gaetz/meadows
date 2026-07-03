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
// Brick 9: analytic sky + day/night cycle — a time-of-day slider drives the
// sun's direction/color, the sky gradient, and the terrain's ambient, all
// through the shared FrameUbo (one sun for everything, fog reuses it later).
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
    render::SkySystem sky;
    bool regenerateRequested { false };
    bool wireframeUi { false };
    bool animateTime { false };

    rhi::BufferHandle frameUbo {};
    rhi::BindGroupHandle frameBindGroup {};
};

} // namespace game
