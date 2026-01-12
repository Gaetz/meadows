#pragma once

#include "../Defines.h"
#include "VulkanContext.h"
#include <vulkan/vulkan.hpp>
#include <vector>
#include <chrono>

#include "Buffer.h"
#include "Camera.h"
#include "ComputeEffect.h"
#include "DeletionQueue.hpp"
#include "DescriptorAllocatorGrowable.h"
#include "MaterialPipeline.h"
#include "RenderObject.h"
#include "Utils.hpp"
#include "VulkanLoader.h"
#include "Pipelines/GLTFMetallicRoughness.h"
#include "Pipelines/ShadowPipeline.h"
#include "ShadowMap.h"

namespace graphics {
    struct Node;

    struct FrameData {
        vk::CommandPool commandPool;
        vk::CommandBuffer mainCommandBuffer;
        vk::Semaphore imageAvailableSemaphore;
        vk::Fence renderFence;
        DeletionQueue deletionQueue;
        DescriptorAllocatorGrowable frameDescriptors;
    };

class Renderer {
public:
    Renderer(VulkanContext* context);
    ~Renderer();

    void init();
    void cleanup();
    void draw();
    void processEvent(const SDL_Event& event);

    GPUMeshBuffers uploadMesh(std::span<uint32_t> indices, std::span<Vertex> vertices);

    VulkanContext* getContext() const { return context; }
    vk::DescriptorSetLayout getSceneDataDescriptorLayout() const { return gpuSceneDataDescriptorLayout; }
    vk::DescriptorSetLayout getShadowSceneDataDescriptorLayout() const { return shadowSceneDataDescriptorLayout; }
    ImmediateSubmitter* getImmediateSubmitter() { return &immSubmitter; }
    ShadowMap* getShadowMap() { return shadowMap.get(); }

    // External DrawContext (from Scene)
    void setDrawContext(DrawContext* ctx) { externalDrawContext = ctx; }
    DrawContext* getDrawContext() { return externalDrawContext ? externalDrawContext : &mainDrawContext; }

    // Rendering data
    Image errorCheckerboardImage;
    Image whiteImage;
    Image blackImage;
    Image greyImage;
    vk::Sampler defaultSamplerLinear;
    vk::Sampler defaultSamplerNearest;
    pipelines::GLTFMetallicRoughness metalRoughMaterial;

private:
    void createCommandPoolAndBuffers();
    void createSyncObjects();
    void createDescriptors();

    void createPipelines();
    void createBackgroundPipeline();
    void createTrianglePipeline();
    void createMeshPipeline();

    void initImGui();
    void drawImGui(vk::CommandBuffer commandBuffer);

    void drawBackground(vk::CommandBuffer);
    void drawGeometry(vk::CommandBuffer);
    void drawShadowPass(vk::CommandBuffer);
    void drawShadowGeometry(vk::CommandBuffer, vk::DescriptorSet sceneDescriptor);
    void drawShadowDebug(vk::CommandBuffer, vk::DescriptorSet sceneDescriptor);
    void updateLightMatrices();

    // Data methods
    float getMinRenderScale() const;
    void createSceneData();
    void updateScene();

    VulkanContext* context;

    bool resizeRequested {false};
    float renderScale { 0.5f };

    int frameNumber {0};
    static const int FRAME_OVERLAP = 2;

    FrameData frames[FRAME_OVERLAP];
    FrameData& getCurrentFrame() { return frames[frameNumber % FRAME_OVERLAP]; };

    // One semaphore per swapchain image for proper synchronization
    std::vector<vk::Semaphore> renderFinishedSemaphores;

    vk::DescriptorSetLayout drawImageDescriptorLayout;
    vk::DescriptorSet drawImageDescriptors;

    ImmediateSubmitter immSubmitter;

    // Compute effects
    vk::PipelineLayout computePipelineLayout;
    std::vector<ComputeEffect> backgroundEffects;
    int currentBackgroundEffect{0};

    // Graphics pipelines
    uptr<MaterialPipeline> trianglePipeline;
    uptr<MaterialPipeline> meshPipeline;

    // Scene data
    Camera mainCamera;
    GPUMeshBuffers rectangleMesh;
    vector<sptr<MeshAsset>> testMeshes;
    GPUSceneData sceneData;
    Buffer sceneDataBuffer;
    vk::DescriptorSetLayout gpuSceneDataDescriptorLayout;
    vk::DescriptorSetLayout shadowSceneDataDescriptorLayout;  // Scene data + shadow map
    vk::DescriptorSetLayout singleImageDescriptorLayout;

    // Shadow mapping
    uptr<ShadowMap> shadowMap;
    pipelines::ShadowPipeline shadowPipeline;
    bool displayShadowMap { false };
    bool enablePCF { true };
    bool animateLight { true };
    Vec3 lightPos { 0.0f, 50.0f, 25.0f };
    float lightFOV { 45.0f };
    float lightAngle { 0.0f };

    MaterialInstance defaultData;
    Buffer defaultMaterialConstants;

    DrawContext mainDrawContext;
    std::unordered_map<std::string, sptr<Node>> loadedNodes;
    std::unordered_map<std::string, std::shared_ptr<LoadedGLTF>> loadedScenes;

    // External DrawContext (from Scene)
    DrawContext* externalDrawContext { nullptr };

    // Optimization data
    MaterialPipeline* lastPipeline { nullptr };
    MaterialInstance* lastMaterial { nullptr };
    vk::Buffer lastIndexBuffer { nullptr };

    // ImGui
    vk::DescriptorPool imguiDescriptorPool;
    float rotationSpeed = 1.0f;
    float accumulatedRotation = 0.0f;
    std::chrono::high_resolution_clock::time_point lastFrameTime;
};

} // namespace graphics
