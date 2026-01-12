#pragma once

#include <SDL3/SDL.h>
#include "Defines.h"
#include "Graphics/Renderer.h"
#include "Graphics/LoadedGLTF.h"
#include "Graphics/Image.h"
#include "Graphics/Techniques/BasicTechnique.h"
#include "Graphics/Techniques/ShadowMappingTechnique.h"
#include "Graphics/Techniques/DeferredRenderingTechnique.h"
#include "Scene.h"

using graphics::VulkanContext;
using graphics::Renderer;

class Engine {
public:
    Engine() = default;
    ~Engine();

    void init();
    void run();
    void cleanup();

    void setActiveScene(Scene* scene);

private:
    void initWindow();
    void initVulkan();
    void initScenes();
    void mainLoop();

    struct SDL_Window* window{ nullptr };
    uptr<VulkanContext> vulkanContext;
    uptr<Renderer> renderer;

    // Scenes
    uptr<Scene> basicScene;
    uptr<Scene> shadowScene;
    uptr<Scene> deferredScene;
    Scene* activeScene { nullptr };

    // Loaded models (kept alive for scenes)
    sptr<graphics::LoadedGLTF> basicSceneModel;
    sptr<graphics::LoadedGLTF> shadowSceneModel;
    sptr<graphics::LoadedGLTF> deferredSceneModel;

    // Rendering techniques (owned by Engine, used by scenes)
    uptr<graphics::techniques::BasicTechnique> basicTechnique;
    uptr<graphics::techniques::ShadowMappingTechnique> shadowMappingTechnique;
    uptr<graphics::techniques::DeferredRenderingTechnique> deferredTechnique;

    // KTX textures for armor model
    std::optional<graphics::Image> armorColorMap;
    std::optional<graphics::Image> armorNormalMap;
    graphics::Buffer armorMaterialBuffer;
};
