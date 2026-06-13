#include <doctest/doctest.h>

#include "game/SceneStack.hpp"

namespace {

// Records its update calls into a shared log; configurable opaque/blocksUpdate.
struct MockScene : game::Scene {
    int id;
    bool isOpaque;
    bool blocks;
    vector<int>* log;
    int* entered;

    MockScene(int id, bool isOpaque, bool blocks, vector<int>* log,
              int* entered = nullptr)
        : id(id), isOpaque(isOpaque), blocks(blocks), log(log),
          entered(entered) {}

    void onEnter() override {
        if (entered) {
            ++*entered;
        }
    }
    void update(f32) override { log->push_back(id); }
    bool opaque() const override { return isOpaque; }
    bool blocksUpdate() const override { return blocks; }
};

uptr<game::Scene> make(int id, bool isOpaque, bool blocks, vector<int>* log,
                       int* entered = nullptr) {
    return uptr<game::Scene> {
        new MockScene(id, isOpaque, blocks, log, entered)
    };
}

} // namespace

TEST_CASE("scene stack: transitions are deferred until applyPending/update") {
    vector<int> log;
    int entered = 0;
    game::SceneStack stack;

    stack.push(make(1, true, true, &log, &entered));
    CHECK(stack.empty());      // not applied yet
    CHECK(entered == 0);

    stack.update(0.0f);        // applies pending, then updates
    CHECK(stack.size() == 1);
    CHECK(entered == 1);
    CHECK(log == vector<int> { 1 });
}

TEST_CASE("scene stack: an opaque, blocking top hides and pauses those below") {
    vector<int> log;
    game::SceneStack stack;
    stack.push(make(1, true, true, &log));
    stack.push(make(2, true, true, &log)); // opaque + blocking
    stack.applyPending();

    CHECK(stack.firstUpdatedIndex() == 1); // only the top
    CHECK(stack.firstDrawnIndex() == 1);

    log.clear();
    stack.update(0.0f);
    CHECK(log == vector<int> { 2 }); // scene 1 paused
}

TEST_CASE("scene stack: a non-blocking overlay lets the scene below update") {
    vector<int> log;
    game::SceneStack stack;
    stack.push(make(1, true, true, &log));
    stack.push(make(2, false, false, &log)); // transparent + non-blocking overlay
    stack.applyPending();

    CHECK(stack.firstUpdatedIndex() == 0); // both update
    CHECK(stack.firstDrawnIndex() == 0);   // both draw

    log.clear();
    stack.update(0.0f);
    CHECK(log == vector<int> { 1, 2 }); // bottom-to-top order
}

TEST_CASE("scene stack: a transparent but blocking overlay draws below, pauses below") {
    vector<int> log;
    game::SceneStack stack;
    stack.push(make(1, true, true, &log));
    stack.push(make(2, false, true, &log)); // transparent (draw below) + blocking (pause below)
    stack.applyPending();

    CHECK(stack.firstUpdatedIndex() == 1); // only top updates
    CHECK(stack.firstDrawnIndex() == 0);   // both drawn
}

TEST_CASE("scene stack: replace swaps the top and fires lifecycle") {
    vector<int> log;
    int entered = 0;
    game::SceneStack stack;
    stack.push(make(1, true, true, &log, &entered));
    stack.applyPending();
    CHECK(entered == 1);

    stack.replace(make(2, true, true, &log, &entered));
    stack.applyPending();
    CHECK(stack.size() == 1);
    CHECK(entered == 2);

    log.clear();
    stack.update(0.0f);
    CHECK(log == vector<int> { 2 }); // scene 1 is gone
}
