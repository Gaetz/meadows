#pragma once

#include "engine/core/Defines.hpp"

namespace engine {
struct FrameContext;
}
namespace render {
class SpriteRenderer;
}

namespace game {

// A layer on the SceneStack. The stack updates/draws from the top down per the
// opaque / blocksUpdate rules below. Independent scenes own their own state
// (their own ecs::World, FormDatabase…); overlays that act on the scene below
// reference a shared session (a later concern). A scene captures whatever it
// needs (e.g. engine::Engine&) at construction, so the lifecycle hooks are
// parameterless.
class Scene {
public:
    virtual ~Scene() = default;

    virtual void onEnter() {}
    virtual void onExit() {}

    virtual void update(f32 /*dt*/) {}
    virtual void draw(render::SpriteRenderer& /*renderer*/) {}
    virtual void drawUi() {}

    // A frame-owning scene records its own render passes (3D: shadow /
    // offscreen / composite chains) and must leave the backbuffer holding the
    // scene on return. The stack then layers the usual sprite pass on top
    // (LoadOp::Load), so 2D overlays keep working above a 3D scene.
    virtual bool ownsFrame() const { return false; }
    virtual void render(engine::FrameContext& /*frame*/) {}

    // If false, this scene is an overlay: the scene below is drawn too.
    virtual bool opaque() const { return true; }
    // If false, the scene below also updates (this scene does not pause it).
    virtual bool blocksUpdate() const { return true; }
};

} // namespace game
