#pragma once

#include <SDL3/SDL.h>
#include <vulkan/vulkan.hpp>
#include <vector>
#include <iostream>

class Engine {
public:
    Engine();
    ~Engine();

    void init();
    void run();
    void cleanup();

private:
    void initWindow();
    void initVulkan();
    void mainLoop();

    bool isInitialized{ false };
    int windowExtent{ 800 };

    struct SDL_Window* window{ nullptr };
    class VulkanContext* vulkanContext{ nullptr };
    class Renderer* renderer{ nullptr };
};
