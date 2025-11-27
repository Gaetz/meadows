#pragma once

#include <SDL3/SDL.h>
#include <vulkan/vulkan.hpp>
#include <vector>
#include <iostream>

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
    class VulkanContext* vulkanContext{ nullptr };
    class Renderer* renderer{ nullptr };
};
