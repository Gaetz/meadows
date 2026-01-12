#include "Engine.h"
#include "BasicServices/Log.h"
#include "Graphics/VulkanContext.h"
#include "Graphics/Renderer.h"
#include "Graphics/Techniques/BasicTechnique.h"
#include "Graphics/Techniques/ShadowMappingTechnique.h"
#include "Graphics/VulkanLoader.h"
#include "Scene.h"
#include <backends/imgui_impl_sdl3.h>

#include "BasicServices/RenderingStats.h"

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

    initScenes();

    Log::Info("Engine Initialized");
}

void Engine::initScenes() {
    // Initialize rendering techniques
    basicTechnique = std::make_unique<graphics::techniques::BasicTechnique>();
    basicTechnique->init(renderer.get());

    shadowMappingTechnique = std::make_unique<graphics::techniques::ShadowMappingTechnique>();
    shadowMappingTechnique->init(renderer.get());

    deferredTechnique = std::make_unique<graphics::techniques::DeferredRenderingTechnique>();
    deferredTechnique->init(renderer.get());

    // Create scene with basic technique (no shadows)
    basicScene = std::make_unique<Scene>(renderer.get());
    basicScene->setRenderingTechnique(basicTechnique.get());

    // Load a model into the basic scene
    auto basicModel = graphics::loadGltf(renderer.get(), "assets/structure.glb");
    if (basicModel.has_value()) {
        basicSceneModel = *basicModel;
        Log::Info("Loaded model for basic scene");
    }

    // Create scene with shadow mapping technique
    shadowScene = std::make_unique<Scene>(renderer.get());
    shadowScene->setRenderingTechnique(shadowMappingTechnique.get());

    // Load a model into the shadow scene (use same model)
    auto shadowModel = graphics::loadGltf(renderer.get(), "assets/structure.glb");
    if (shadowModel.has_value()) {
        shadowSceneModel = *shadowModel;
        Log::Info("Loaded model for shadow scene");
    }

    // Create scene with deferred technique
    deferredScene = std::make_unique<Scene>(renderer.get());
    deferredScene->setRenderingTechnique(deferredTechnique.get());

    // Load a model into the deferred scene
    auto deferredModel = graphics::loadGltf(renderer.get(), "assets/structure.glb");
    if (deferredModel.has_value()) {
        deferredSceneModel = *deferredModel;
        Log::Info("Loaded model for deferred scene");
    }

    // Set the default active scene (with deferred rendering)
    setActiveScene(shadowScene.get());
}

void Engine::setActiveScene(Scene* scene) {
    activeScene = scene;
    if (activeScene) {
        renderer->setDrawContext(&activeScene->getDrawContext());
        renderer->setRenderingTechnique(activeScene->getRenderingTechnique());
        renderer->setActiveScene(activeScene);

        // Disable light animation for the basic scene to prevent color/shading changes
        if (activeScene == basicScene.get()) {
            renderer->setAnimateLight(false);
        } else {
            renderer->setAnimateLight(true);
        }
    } else {
        renderer->setDrawContext(nullptr);
        renderer->setRenderingTechnique(nullptr);
        renderer->setActiveScene(nullptr);
    }
}

void Engine::cleanup() {
    if (!renderer) {
        return; // Already cleaned up or never initialized
    }

    // Wait for device to be idle before destroying renderer
    vulkanContext->getDevice().waitIdle();

    // Cleanup loaded models
    basicSceneModel.reset();
    shadowSceneModel.reset();
    deferredSceneModel.reset();

    // Cleanup scenes
    basicScene.reset();
    shadowScene.reset();
    deferredScene.reset();
    activeScene = nullptr;

    // Cleanup rendering techniques
    vk::Device device = vulkanContext->getDevice();
    if (basicTechnique) {
        basicTechnique->cleanup(device);
        basicTechnique.reset();
    }
    if (shadowMappingTechnique) {
        shadowMappingTechnique->cleanup(device);
        shadowMappingTechnique.reset();
    }
    if (deferredTechnique) {
        deferredTechnique->cleanup(device);
        deferredTechnique.reset();
    }

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
        // Begin stats clock
        auto start = std::chrono::system_clock::now();

        // Handle events on queue
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);

            if (e.type == SDL_EVENT_QUIT) {
                quit = true;
            }

            // Pass events to renderer for camera control
            renderer->processEvent(e);
        }

        // Update active scene DrawContext with loaded model
        if (activeScene) {
            activeScene->getDrawContext().opaqueSurfaces.clear();
            activeScene->getDrawContext().transparentSurfaces.clear();

            if (activeScene == basicScene.get() && basicSceneModel) {
                basicSceneModel->draw(Mat4{1.f}, activeScene->getDrawContext());
            } else if (activeScene == shadowScene.get() && shadowSceneModel) {
                shadowSceneModel->draw(Mat4{1.f}, activeScene->getDrawContext());
            } else if (activeScene == deferredScene.get() && deferredSceneModel) {
                deferredSceneModel->draw(Mat4{1.f}, activeScene->getDrawContext());
            }
        }

        renderer->draw();

        // End stats clock
        auto end = std::chrono::system_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        services::RenderingStats::Instance().frameTime = static_cast<float>(elapsed.count()) / 1000.f;
    }

    vulkanContext->getDevice().waitIdle();
}
