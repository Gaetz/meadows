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
    // §2.1 preference chain: try the configured backend (Vulkan by default),
    // fall back to GL. The SDL surface flag is fixed at window creation and
    // must match the backend, so falling back means recreating the window —
    // acceptable at init, and the reason the chain lives HERE and not in the
    // Device factory.
    const platform::GraphicsApi api =
        config.backend == rhi::Backend::Vulkan ? platform::GraphicsApi::Vulkan
                                                : platform::GraphicsApi::OpenGL;
    window = platform::Window::create({ .title = config.title,
                                        .width = config.width,
                                        .height = config.height,
                                        .api = api });
    if (!window) {
        return false;
    }
    device = rhi::Device::create(config.backend, *window);
    if (!device && config.backend == rhi::Backend::Vulkan) {
        LOG_WARN("Vulkan unavailable — falling back to OpenGL");
        window.reset(); // surface flag mismatch: a GL device needs a GL window
        window = platform::Window::create({ .title = config.title,
                                            .width = config.width,
                                            .height = config.height,
                                            .api = platform::GraphicsApi::OpenGL });
        if (!window) {
            return false;
        }
        device = rhi::Device::create(rhi::Backend::OpenGL, *window);
    }
    if (!device) {
        return false;
    }
    LOG_INFO("RHI backend: {}",
             device->backend() == rhi::Backend::Vulkan ? "Vulkan" : "OpenGL");
    spriteRenderer = render::SpriteRenderer::create(*device);
    if (!spriteRenderer) {
        return false;
    }
    imgui = ui::ImGuiLayer::create(*window, *device);
    if (!imgui) {
        return false;
    }
    // One event hook, fanned out: ImGui (dev UI) first, then the event-fed
    // Input channel (text/wheel/key events for the game UI).
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

        // Dev UI renders after the scene, into the backbuffer, inside its
        // own Load pass: GL tolerated bare draws, Vulkan requires an active
        // render pass — Load preserves whatever the scene just rendered.
        imgui->beginFrame();
        game.drawUi();
        cmd.beginRenderPass({ .loadOp = rhi::LoadOp::Load,
                              .depthLoadOp = rhi::LoadOp::DontCare });
        imgui->render(cmd);
        cmd.endRenderPass();

        device->endFrame();
    }
}

} // namespace engine
