#include <memory>

#include <imgui.h>

#include "engine/Engine.hpp"
#include "engine/Game.hpp"
#include "game/SceneStack.hpp"
#include "game/scenes/DemoScenes.hpp"

namespace {

// The app: a scene stack plus a small selector to switch between the isolated
// demos. The engine still drives a single engine::Game; the scene stack lives
// above it (meadows-runtime), so each system is exercised on its own scene.
class DemoApp final : public engine::Game {
public:
    void init(engine::Engine& engineContext) override {
        engine = &engineContext;
        stack.replace(std::make_unique<game::PluginScene>(*engine));
    }

    void update(f32 dt) override { stack.update(dt); }

    void draw(render::SpriteRenderer& renderer) override { stack.draw(renderer); }

    void drawUi() override {
        ImGui::Begin("Demos");
        if (ImGui::Button("Plugins / mods")) {
            stack.replace(std::make_unique<game::PluginScene>(*engine));
        }
        ImGui::SameLine();
        if (ImGui::Button("World editor")) {
            stack.replace(std::make_unique<game::WorldEditScene>(*engine));
        }
        ImGui::SameLine();
        if (ImGui::Button("Combat")) {
            stack.replace(std::make_unique<game::CombatScene>(*engine));
        }
        ImGui::End();

        stack.drawUi();
    }

    void close() override {
        // Run each scene's onExit (frees GPU) while the device is still alive.
        while (!stack.empty()) {
            stack.pop();
            stack.applyPending();
        }
    }

private:
    engine::Engine* engine { nullptr };
    game::SceneStack stack;
};

} // namespace

int main(int /*argc*/, char** /*argv*/) {
    DemoApp game;
    return engine::Engine::run({ .title = "True Adventurer" }, game);
}
