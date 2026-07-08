#include "engine/Engine.hpp"

#include <algorithm>

#include "engine/FrameContext.hpp"
#include "engine/Game.hpp"
#include "engine/core/Clock.hpp"
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
    if (!imgui) {
        return false;
    }
    // One event hook, fanned out: ImGui (dev UI) first, then the event-fed
    // Input channel (text/wheel/key events for the game UI, chantier 4).
    window->setEventHook([this](const void* nativeEvent) {
        ui::ImGuiLayer::processEvent(nativeEvent);
        input.handleEvent(nativeEvent);
    });
    return true;
}

void Engine::loop(Game& game) {
    // Variable timestep for now; a fixed simulation step with render
    // interpolation slots in here once gameplay needs determinism (§8).
    auto lastTime = core::clockNow();
    while (window->pumpEvents() && !quitRequested) {
        const auto now = core::clockNow();
        const f32 dt = std::min(
            static_cast<f32>(core::secondsBetween(lastTime, now)), kMaxDt);
        lastTime = now;

        input.update(); // snapshot keyboard state for this frame's update
        game.update(dt);

        auto& cmd = device->beginFrame();
        const f32 aspect = static_cast<f32>(window->width()) /
                           static_cast<f32>(window->height());
        FrameContext frame { .device = *device,
                             .cmd = cmd,
                             .sprites = *spriteRenderer,
                             .camera2d = camera,
                             .clearColor = config.clearColor,
                             .dt = dt,
                             .aspect = aspect,
                             .width = static_cast<u32>(window->width()),
                             .height = static_cast<u32>(window->height()) };
        game.render(frame);

        // Dev UI renders after the scene, straight into the backbuffer.
        imgui->beginFrame();
        game.drawUi();
        imgui->render();

        device->endFrame();
    }
}

} // namespace engine
