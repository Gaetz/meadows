#include "game/SceneStack.hpp"

#include "engine/FrameContext.hpp"
#include "engine/render/SpriteRenderer.hpp"
#include "engine/rhi/CommandBuffer.hpp"

namespace game {

void SceneStack::push(uptr<Scene> scene) {
    pending.push_back({ Op::Push, std::move(scene) });
}

void SceneStack::pop() {
    pending.push_back({ Op::Pop, nullptr });
}

void SceneStack::replace(uptr<Scene> scene) {
    pending.push_back({ Op::Replace, std::move(scene) });
}

void SceneStack::applyPending() {
    for (Pending& op : pending) {
        switch (op.op) {
        case Op::Pop:
            if (!scenes.empty()) {
                scenes.back()->onExit();
                scenes.pop_back();
            }
            break;
        case Op::Replace:
            if (!scenes.empty()) {
                scenes.back()->onExit();
                scenes.pop_back();
            }
            scenes.push_back(std::move(op.scene));
            scenes.back()->onEnter();
            break;
        case Op::Push:
            scenes.push_back(std::move(op.scene));
            scenes.back()->onEnter();
            break;
        }
    }
    pending.clear();
}

size_t SceneStack::firstUpdatedIndex() const {
    size_t i = scenes.size() - 1;
    while (i > 0 && !scenes[i]->blocksUpdate()) {
        --i;
    }
    return i;
}

size_t SceneStack::firstDrawnIndex() const {
    size_t i = scenes.size() - 1;
    while (i > 0 && !scenes[i]->opaque()) {
        --i;
    }
    return i;
}

void SceneStack::update(f32 dt) {
    applyPending();
    if (scenes.empty()) {
        return;
    }
    for (size_t i = firstUpdatedIndex(); i < scenes.size(); ++i) {
        scenes[i]->update(dt);
    }
}

void SceneStack::render(engine::FrameContext& frame) {
    const size_t first = scenes.empty() ? 0 : firstDrawnIndex();
    const bool frameOwned = !scenes.empty() && scenes[first]->ownsFrame();
    if (frameOwned) {
        scenes[first]->render(frame);
    }

    // Sprite pass over every drawn scene (a frame owner's default draw() is a
    // no-op). Load preserves the owner's backbuffer; a pure-2D stack clears,
    // exactly as the pre-seam loop did.
    frame.cmd.beginRenderPass({ .loadOp = frameOwned ? rhi::LoadOp::Load
                                                     : rhi::LoadOp::Clear,
                                .clearColor = frame.clearColor });
    frame.sprites.begin(frame.camera2d, frame.aspect);
    for (size_t i = first; i < scenes.size(); ++i) {
        scenes[i]->draw(frame.sprites);
    }
    frame.sprites.end(frame.cmd);
    frame.cmd.endRenderPass();
}

void SceneStack::draw(render::SpriteRenderer& renderer) {
    if (scenes.empty()) {
        return;
    }
    for (size_t i = firstDrawnIndex(); i < scenes.size(); ++i) {
        scenes[i]->draw(renderer);
    }
}

void SceneStack::drawUi() {
    if (scenes.empty()) {
        return;
    }
    for (size_t i = firstDrawnIndex(); i < scenes.size(); ++i) {
        scenes[i]->drawUi();
    }
}

} // namespace game
