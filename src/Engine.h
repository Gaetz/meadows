#pragma once

#include <SDL3/SDL.h>
#include "Defines.h"
#include "Graphics/Renderer.h"
#include "IScene.h"
#include "GltfScene.h"
#include "Graphics/Techniques/BasicTechnique.h"
#include "Graphics/Techniques/ShadowMappingTechnique.h"
#include "Graphics/Techniques/DeferredRenderingTechnique.h"
#include "Graphics/Techniques/RainbowTechnique.h"
#include "RainbowScene.h"

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

    // Scenes
    uptr<IScene> basicScene;
    uptr<IScene> shadowScene;
    uptr<IScene> deferredScene;
    uptr<IScene> rainbowScene;
    IScene* activeScene { nullptr };

    // Rendering techniques (owned by Engine, used by scenes)
    uptr<graphics::techniques::BasicTechnique> basicTechnique;
    uptr<graphics::techniques::ShadowMappingTechnique> shadowMappingTechnique;
    uptr<graphics::techniques::DeferredRenderingTechnique> deferredTechnique;
    uptr<graphics::techniques::RainbowTechnique> rainbowTechnique;
};
