#pragma once

#include "engine/core/Defines.hpp"
#include "game/Scene.hpp"

namespace render {
class SpriteRenderer;
}

namespace game {

// A stack of scenes (the app/harness layer; lives above the engine in
// meadows-runtime). Transitions are **deferred** — push/pop/replace queue an
// op applied at the start of the next update — so the stack is never mutated
// mid update/draw. Update covers the top down to (and including) the first
// scene whose blocksUpdate() is true; draw/drawUi cover the top down to the
// first opaque() scene. Active scenes run bottom-to-top.
class SceneStack {
public:
    void push(uptr<Scene> scene);
    void pop();
    void replace(uptr<Scene> scene); // pop the top, then push this
    void applyPending();             // applies queued ops (fires onEnter/onExit)

    void update(f32 dt);             // applies pending, then updates the active set
    void draw(render::SpriteRenderer& renderer);
    void drawUi();

    bool empty() const { return scenes.empty(); }
    size_t size() const { return scenes.size(); }
    Scene* top() const { return scenes.empty() ? nullptr : scenes.back().get(); }

    // Lowest index that receives update / draw. Valid only when !empty().
    size_t firstUpdatedIndex() const;
    size_t firstDrawnIndex() const;

private:
    enum class Op { Push, Pop, Replace };
    struct Pending {
        Op op;
        uptr<Scene> scene;
    };

    vector<uptr<Scene>> scenes;
    vector<Pending> pending;
};

} // namespace game
