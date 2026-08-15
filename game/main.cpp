#include <functional>
#include <memory>

#include <imgui.h>

#include "engine/Engine.hpp"
#include "engine/FrameContext.hpp"
#include "engine/Game.hpp"
#include "engine/rhi/Device.hpp"
#include "game/SceneStack.hpp"
#include "game/scenes/CombatArenaScene.hpp"
#include "game/scenes/EditorScene.hpp"
#include "game/scenes/LandscapeScene.hpp"
#include "game/scenes/StatsScene.hpp"

namespace {

// The app: a scene stack plus a small selector to switch between the isolated
// demos. The engine still drives a single engine::Game; the scene stack lives
// above it (meadows-runtime), so each system is exercised on its own scene.
class DemoApp final : public engine::Game {
public:
    void init(engine::Engine& engineContext) override {
        engine = &engineContext;
        stack.replace(std::make_unique<game::LandscapeScene>(*engine));
    }

    void update(f32 dt) override { stack.update(dt); }

    void render(engine::FrameContext& frame) override { stack.render(frame); }

    void drawUi() override {
        // AlwaysAutoResize so the window grows to fit every button (ignoring any
        // stale saved size in imgui.ini) — no demo is clipped as the list grows.
        ImGui::Begin("Demos", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

        struct Demo {
            const char* label;
            std::function<std::unique_ptr<game::Scene>()> make;
        };
        const Demo demos[] = {
            { "Combat arena",
              [&] { return std::make_unique<game::CombatArenaScene>(*engine); } },
            { "Stats",
              [&] { return std::make_unique<game::StatsScene>(*engine); } },
            { "Landscape (3D)",
              [&] { return std::make_unique<game::LandscapeScene>(*engine); } },
            { "Game DB (editor)",
              [&] { return std::make_unique<game::EditorScene>(*engine); } },
        };

        // Wrap buttons across rows at a fixed budget. A fixed budget (not the
        // window width) avoids a feedback loop with AlwaysAutoResize.
        const ImGuiStyle& style = ImGui::GetStyle();
        constexpr float kWrapWidth = 520.0f;
        float rowX = 0.0f;
        for (int i = 0; i < IM_ARRAYSIZE(demos); ++i) {
            const float w = ImGui::CalcTextSize(demos[i].label).x +
                            style.FramePadding.x * 2.0f;
            if (i > 0) {
                if (rowX + style.ItemSpacing.x + w <= kWrapWidth) {
                    ImGui::SameLine();
                    rowX += style.ItemSpacing.x;
                } else {
                    rowX = 0.0f; // overflow: start a new row
                }
            }
            if (ImGui::Button(demos[i].label)) {
                stack.replace(demos[i].make());
            }
            rowX += w;
        }
        // Active RHI backend (§2.1 runtime chain): makes the Vulkan-vs-GL
        // fallback visible at a glance in every scene.
        ImGui::TextDisabled("Backend: %s",
                            engine->getDevice().backend() ==
                                    rhi::Backend::Vulkan
                                ? "Vulkan"
                                : "OpenGL");
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
