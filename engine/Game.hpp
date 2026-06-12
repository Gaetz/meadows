#pragma once

#include "engine/core/Defines.hpp"

namespace render {
class SpriteRenderer;
}

namespace engine {

class Engine;

// The game's hooks into the frame loop. The Engine owns the loop itself:
// fixed timestep, frame pacing, and future render threading are engine
// concerns the game never sees.
//
// Discipline that keeps future render architectures open:
//   - update() must not touch rendering.
//   - draw()/drawUi() must not mutate game state (read and submit only).
class Game {
public:
    virtual ~Game() = default;

    // Called once, after every engine system is up.
    virtual void init(Engine& engine) = 0;

    // Simulation step. `dt` is in seconds, clamped by the engine.
    virtual void update(f32 dt) = 0;

    // Submit this frame's sprites, in painter's order.
    virtual void draw(render::SpriteRenderer& renderer) = 0;

    // Emit dev-UI widgets (ImGui). Optional.
    virtual void drawUi() {}

    // Called once, before engine systems go down.
    virtual void close() = 0;
};

} // namespace engine
