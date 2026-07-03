#pragma once

#include "game/Scene.hpp"

namespace engine {
class Engine;
}

namespace game {

// The 3D landscape renderer prototype (custom-renderer path, Phases 11-14).
// Owns the frame: records its own render passes instead of the sprite path.
// Brick 1: an empty frame owner proving the Engine/Scene seam — a sky-blue
// clear pass and a debug panel. The LandscapeRenderer arrives brick by brick.
class LandscapeScene final : public Scene {
public:
    explicit LandscapeScene(engine::Engine& engineContext)
        : engine(&engineContext) {}

    bool ownsFrame() const override { return true; }
    void render(engine::FrameContext& frame) override;
    void drawUi() override;

private:
    engine::Engine* engine { nullptr };
};

} // namespace game
