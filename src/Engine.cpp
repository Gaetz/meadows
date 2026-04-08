#include "Engine.h"
#include "BasicServices/Log.h"
#include "Graphics/VulkanContext.h"
#include "Graphics/Renderer.h"
#include "BasicScene.h"
#include "ShadowScene.h"
#include "DeferredScene.h"
#include "RainbowScene.h"
#include <backends/imgui_impl_sdl3.h>
#include "BasicServices/RenderingStats.h"

using services::Log;


Engine::~Engine() {
    cleanup();
}

void Engine::init() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        Log::Error("Failed to initialize SDL: %s", SDL_GetError());
        return;
    }

    initWindow();
    initVulkan();

    renderer = std::make_unique<Renderer>(vulkanContext.get());
    renderer->init();

    initScenes();

    Log::Info("Engine Initialized");
}

void Engine::initScenes() {
    basicScene    = std::make_unique<BasicScene>(renderer.get());
    shadowScene   = std::make_unique<ShadowScene>(renderer.get());
    deferredScene = std::make_unique<DeferredScene>(renderer.get());
    rainbowScene  = std::make_unique<RainbowScene>(renderer.get());

    renderer->setImguiCallback([this]() {
        if (ImGui::Begin("Scenes")) {
            if (ImGui::Button("Basic"))    setActiveScene(basicScene.get());
            if (ImGui::Button("Shadow"))   setActiveScene(shadowScene.get());
            if (ImGui::Button("Deferred")) setActiveScene(deferredScene.get());
            if (ImGui::Button("Rainbow"))  setActiveScene(rainbowScene.get());
        }
        ImGui::End();
    });

    setActiveScene(rainbowScene.get());
}

void Engine::setActiveScene(IScene* scene) {
    activeScene = scene;
    if (activeScene) {
        renderer->setDrawContext(&activeScene->getDrawContext());
        renderer->setRenderingTechnique(activeScene->getRenderingTechnique());
        renderer->setActiveScene(activeScene);
        activeScene->onActivated(renderer.get());
    } else {
        renderer->setDrawContext(nullptr);
        renderer->setRenderingTechnique(nullptr);
        renderer->setActiveScene(nullptr);
    }
}

void Engine::cleanup() {
    if (!renderer) {
        return;
    }

    vulkanContext->getDevice().waitIdle();

    // Scenes own and clean up their techniques in their destructors
    basicScene.reset();
    shadowScene.reset();
    deferredScene.reset();
    rainbowScene.reset();
    activeScene = nullptr;

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
    window = SDL_CreateWindow(
        "Vulkan Engine",
        1720, 720,
        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE
    );

    if (!window) {
        Log::Error("Failed to create window: %s", SDL_GetError());
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
        auto start = std::chrono::system_clock::now();

        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);

            if (e.type == SDL_EVENT_QUIT) {
                quit = true;
            }

            renderer->processEvent(e);
        }

        if (activeScene) {
            activeScene->update();
        }

        renderer->draw();

        auto end = std::chrono::system_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        services::RenderingStats::Instance().frameTime = static_cast<float>(elapsed.count()) / 1000.f;
    }

    vulkanContext->getDevice().waitIdle();
}
