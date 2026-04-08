#include "Engine.h"
#include "BasicServices/Log.h"
#include "Graphics/VulkanContext.h"
#include "Graphics/Renderer.h"
#include "GltfScene.h"
#include "RainbowScene.h"
#include "Graphics/Techniques/RainbowTechnique.h"
#include <backends/imgui_impl_sdl3.h>
#include "BasicServices/RenderingStats.h"
#include <glm/gtx/transform.hpp>

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
    // Initialize rendering techniques
    basicTechnique = std::make_unique<graphics::techniques::BasicTechnique>();
    basicTechnique->init(renderer.get());

    shadowMappingTechnique = std::make_unique<graphics::techniques::ShadowMappingTechnique>();
    shadowMappingTechnique->init(renderer.get());

    deferredTechnique = std::make_unique<graphics::techniques::DeferredRenderingTechnique>();
    deferredTechnique->init(renderer.get());

    // Basic scene
    auto basic = std::make_unique<GltfScene>(renderer.get(), "assets/structure.glb");
    basic->setRenderingTechnique(basicTechnique.get());
    basic->animateLight = false;
    basicScene = std::move(basic);

    // Shadow scene
    auto shadow = std::make_unique<GltfScene>(renderer.get(), "assets/vulkanscene_shadow.gltf");
    shadow->setRenderingTechnique(shadowMappingTechnique.get());
    shadow->initialCameraPos   = Vec3(0.0f, 5.0f, 10.0f);
    shadow->initialCameraPitch = glm::radians(-15.0f);
    shadow->initialCameraYaw   = 0.0f;
    shadow->animateLight       = true;
    shadowScene = std::move(shadow);

    // Deferred scene (armor model with KTX textures)
    auto deferred = std::make_unique<GltfScene>(
        renderer.get(),
        "assets/armor/armor.gltf",
        glm::scale(Vec3(30.0f)) * glm::translate(Vec3(0.0f, 2.3f, 0.0f))
    );
    deferred->setRenderingTechnique(deferredTechnique.get());
    deferred->initialCameraPos = Vec3(0.0f, 50.0f, 100.0f);
    deferred->animateLight     = false;
    deferred->loadKTXColorMap("assets/armor/colormap_rgba.ktx");
    deferredScene = std::move(deferred);

    // Rainbow scene
    rainbowTechnique = std::make_unique<graphics::techniques::RainbowTechnique>();
    rainbowTechnique->init(renderer.get());
    rainbowScene = std::make_unique<RainbowScene>(rainbowTechnique.get());

    // Scene selector UI registered with Renderer
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

    basicScene.reset();
    shadowScene.reset();
    deferredScene.reset();
    rainbowScene.reset();
    activeScene = nullptr;

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
    if (rainbowTechnique) {
        rainbowTechnique->cleanup(device);
        rainbowTechnique.reset();
    }

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
