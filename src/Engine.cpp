#include "Engine.h"
#include "BasicServices/Log.h"
#include "Graphics/VulkanContext.h"
#include "Graphics/Renderer.h"
#include <backends/imgui_impl_sdl3.h>

Engine::Engine() {
    // Constructor
}

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

    renderer = new Renderer(vulkanContext);
    renderer->init();

    isInitialized = true;
    Log::Info("Engine Initialized");
}

void Engine::cleanup() {
    if (isInitialized) {
        if (renderer) {
            // Wait for device to be idle before destroying renderer
            vulkanContext->getDevice().waitIdle();
            delete renderer;
            renderer = nullptr;
        }

        if (vulkanContext) {
            delete vulkanContext;
            vulkanContext = nullptr;
        }

        if (window) {
            SDL_DestroyWindow(window);
        }
        SDL_Quit();
        isInitialized = false;
        Log::Info("Engine Cleaned Up");
    }
}

void Engine::run() {
    if (!isInitialized) {
        return;
    }

    mainLoop();
}

void Engine::initWindow() {
    // Create window with Vulkan flag
    window = SDL_CreateWindow(
        "Vulkan Engine",
        800, 600,
        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE
    );

    if (!window) {
        Log::Error("Failed to create window: %s", SDL_GetError());
        // Handle error appropriately
    }
}

void Engine::initVulkan() {
    vulkanContext = new VulkanContext(window);
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
        }

        if (renderer) {
            renderer->draw();
        }
    }
    
    if (vulkanContext) {
        vulkanContext->getDevice().waitIdle();
    }
}
