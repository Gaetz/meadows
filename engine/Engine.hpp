#pragma once

#include "engine/core/Defines.hpp"
#include "engine/platform/Input.hpp"
#include "engine/render/Camera2D.hpp"
#include "engine/rhi/Rhi.hpp"

namespace core {
class JobSystem;
}
namespace platform {
class Window;
}
namespace rhi {
class Device;
}
namespace render {
class SpriteRenderer;
}
namespace ui {
class ImGuiLayer;
}

namespace engine {

class Game;

struct EngineConfig {
    str title { "Meadows" };
    // Full HD by default — the DB editor needs the room and the game
    // reads better too.
    i32 width { 1920 };
    i32 height { 1080 };
    // Vulkan first (the FINAL renderer — docs/VULKAN.md); Engine::init
    // falls back to GL at runtime if it is unavailable or not compiled in.
    rhi::Backend backend { rhi::Backend::Vulkan };
    rhi::Color clearColor { 0.10f, 0.12f, 0.16f, 1.0f };
};

// Owns the platform window, the RHI device, the renderers, the dev UI and
// the frame loop. The small explicit context passed by reference (§8) — no
// global singletons.
class Engine {
public:
    // Brings every system up, runs the loop until quit, tears down.
    // Returns the process exit code.
    static i32 run(const EngineConfig& config, Game& game);

    ~Engine();

    platform::Window& getWindow() { return *window; }
    rhi::Device& getDevice() { return *device; }
    render::SpriteRenderer& getSpriteRenderer() { return *spriteRenderer; }
    render::Camera2D& getCamera() { return camera; }
    platform::Input& getInput() { return input; }
    core::JobSystem& getJobSystem() { return *jobSystem; }

    // Ends the frame loop after the current frame (the in-game Quit
    // button — Escape belongs to the game UI, not the engine).
    void requestQuit() { quitRequested = true; }

private:
    Engine() = default;

    bool init(const EngineConfig& config);
    void loop(Game& game);

    EngineConfig config;
    uptr<core::JobSystem> jobSystem;
    uptr<platform::Window> window;
    uptr<rhi::Device> device;
    uptr<render::SpriteRenderer> spriteRenderer;
    uptr<ui::ImGuiLayer> imgui;
    render::Camera2D camera;
    platform::Input input;
    bool quitRequested { false };
};

} // namespace engine
