#include "engine/Engine.hpp"

#include <algorithm>
#include <chrono>

#include "engine/Game.hpp"
#include "engine/core/Jobs.hpp"
#include "engine/core/Log.hpp"
#include "engine/platform/Window.hpp"
#include "engine/render/SpriteRenderer.hpp"
#include "engine/rhi/Device.hpp"
#include "engine/ui/ImGuiLayer.hpp"

namespace engine {

namespace {
// Stalls (debugger, window drag) would otherwise produce a giant dt.
constexpr f32 kMaxDt = 0.1f;
} // namespace

Engine::~Engine() = default;

i32 Engine::run(const EngineConfig& config, Game& game) {
    core::Log::init();
    LOG_INFO("meadows engine starting: {}", config.title);

    Engine engine;
    if (!engine.init(config)) {
        return 1;
    }
    game.init(engine);
    engine.loop(game);
    game.close();

    LOG_INFO("meadows engine shutting down");
    return 0;
}

bool Engine::init(const EngineConfig& engineConfig) {
    config = engineConfig;

    jobSystem = std::make_unique<core::JobSystem>();
    window = platform::Window::create({ .title = config.title,
                                        .width = config.width,
                                        .height = config.height });
    if (!window) {
        return false;
    }
    device = rhi::Device::create(config.backend, *window);
    if (!device) {
        return false;
    }
    spriteRenderer = render::SpriteRenderer::create(*device);
    if (!spriteRenderer) {
        return false;
    }
    imgui = ui::ImGuiLayer::create(*window);
    return imgui != nullptr;
}

void Engine::loop(Game& game) {
    // Variable timestep for now; a fixed simulation step with render
    // interpolation slots in here once gameplay needs determinism (§8).
    auto lastTime = std::chrono::steady_clock::now();
    while (window->pumpEvents()) {
        const auto now = std::chrono::steady_clock::now();
        const f32 dt = std::min(
            std::chrono::duration<f32>(now - lastTime).count(), kMaxDt);
        lastTime = now;

        game.update(dt);

        auto& cmd = device->beginFrame();
        cmd.beginRenderPass({ .loadOp = rhi::LoadOp::Clear,
                              .clearColor = config.clearColor });
        const f32 aspect = static_cast<f32>(window->width()) /
                           static_cast<f32>(window->height());
        spriteRenderer->begin(camera, aspect);
        game.draw(*spriteRenderer);
        spriteRenderer->end(cmd);
        cmd.endRenderPass();

        // Dev UI renders after the scene, straight into the backbuffer.
        imgui->beginFrame();
        game.drawUi();
        imgui->render();

        device->endFrame();
    }
}

} // namespace engine
