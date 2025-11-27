#pragma once

#include <SDL3/SDL.h>
#include "Defines.h"
#include "Graphics/Renderer.h"

using graphics::VulkanContext;
using graphics::Renderer;

class Engine {
public:
    Engine() = default;
    ~Engine();

    void init();
    void run();
    void cleanup();

private:
    void initWindow();
    void initVulkan();
    void mainLoop();

    struct SDL_Window* window{ nullptr };
    uptr<VulkanContext> vulkanContext;
    uptr<Renderer> renderer;
};
