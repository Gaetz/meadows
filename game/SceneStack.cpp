#include "game/SceneStack.hpp"

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
