#include "game/SceneStack.hpp"

#include "engine/FrameContext.hpp"
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
            scenes.back()->host_ = this; // scenes push overlays through us
            scenes.back()->onEnter();
            break;
        case Op::Push:
            scenes.push_back(std::move(op.scene));
            scenes.back()->host_ = this;
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
    if (!scenes.empty()) {
        Scene& first = *scenes[firstDrawnIndex()];
        if (first.ownsFrame()) {
            first.render(frame);
            return;
        }
    }
    // No frame owner: clear the backbuffer (the dev ImGui layer draws
    // after, with Load).
    frame.cmd.beginRenderPass({ .loadOp = rhi::LoadOp::Clear,
                                .clearColor = frame.clearColor });
    frame.cmd.endRenderPass();
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
