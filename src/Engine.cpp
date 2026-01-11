#include "Engine.h"
#include "BasicServices/Log.h"
#include "Graphics/VulkanContext.h"
#include "Graphics/Renderer.h"
#include <backends/imgui_impl_sdl3.h>

using services::Log;


Engine::~Engine() {
    cleanup();
}

void Engine::init() {
    // Initialize SDL
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        Log::Error("Failed to initialize SDL: %s", SDL_GetError());
        return;
    }

    initWindow();
    initVulkan();

    renderer = std::make_unique<Renderer>(vulkanContext.get());
    renderer->init();

    Log::Info("Engine Initialized");
}

void Engine::cleanup() {
    if (!renderer) {
        return; // Already cleaned up or never initialized
    }

    // Wait for device to be idle before destroying renderer
    vulkanContext->getDevice().waitIdle();

    // unique_ptr automatically deletes when reset or goes out of scope
    renderer.reset();
    vulkanContext.reset();

    SDL_DestroyWindow(window);
    window = nullptr;

    Log::Info("Engine Cleaned Up");
    SDL_Quit();
}

void Engine::run() {
    if (!renderer) {
        Log::Critical("Engine not initialized. Quitting before the main loop.");
        return;
    }

    mainLoop();
}

void Engine::initWindow() {
    // Create window with Vulkan flag
    window = SDL_CreateWindow(
        "Vulkan Engine",
        1720, 720,
        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE
    );

    if (!window) {
        Log::Error("Failed to create window: %s", SDL_GetError());
        // Handle error appropriately
    }
}

void Engine::initVulkan() {
    vulkanContext = std::make_unique<VulkanContext>(window);
    try {
        vulkanContext->init();
    } catch (const std::exception& e) {
        Log::Error("Vulkan Initialization Error: %s", e.what());
        cleanup();
        exit(-1);
    }
}

void Engine::mainLoop() {
    bool quit = false;
    SDL_Event e;

    while (!quit) {
        // Handle events on queue
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);

            if (e.type == SDL_EVENT_QUIT) {
                quit = true;
            }

            // Pass events to renderer for camera control
            renderer->processEvent(e);
        }

        renderer->draw();
    }
    
    vulkanContext->getDevice().waitIdle();
}
