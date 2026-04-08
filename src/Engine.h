#pragma once

#include <SDL3/SDL.h>
#include "Defines.h"
#include "Graphics/Renderer.h"
#include "IScene.h"

using graphics::VulkanContext;
using graphics::Renderer;

class Engine {
public:
    Engine() = default;
    ~Engine();

    void init();
    void run();
    void cleanup();

    void setActiveScene(IScene* scene);

private:
    void initWindow();
    void initVulkan();
    void initScenes();
    void mainLoop();

    struct SDL_Window* window{ nullptr };
    uptr<VulkanContext> vulkanContext;
    uptr<Renderer> renderer;

    uptr<IScene> basicScene;
    uptr<IScene> shadowScene;
    uptr<IScene> deferredScene;
    uptr<IScene> rainbowScene;
    IScene* activeScene { nullptr };
};
