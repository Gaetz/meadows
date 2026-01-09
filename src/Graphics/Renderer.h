#pragma once

#include "../Defines.h"
#include "VulkanContext.h"
#include <vulkan/vulkan.hpp>
#include <vector>
#include <chrono>

#include "Buffer.h"
#include "ComputeEffect.h"
#include "DeletionQueue.hpp"
#include "DescriptorAllocatorGrowable.h"
#include "MaterialPipeline.h"
#include "Utils.hpp"
#include "VulkanLoader.h"
#include "Pipelines/GLTFMetallicRoughness.h"

namespace graphics {
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

    GPUMeshBuffers uploadMesh(std::span<uint32_t> indices, std::span<Vertex> vertices);

    VulkanContext* getContext() const { return context; }
    vk::DescriptorSetLayout getSceneDataDescriptorLayout() const { return gpuSceneDataDescriptorLayout; }

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

    // Data methods
    float getMinRenderScale() const;
    void createSceneData();


    VulkanContext* context;

    bool resizeRequested {false};
    vk::Extent2D drawExtent;
    float renderScale { 0.5f };

    int frameNumber {0};
    uint32_t currentFrame = 0;
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
    GPUMeshBuffers rectangleMesh;
    vector<sptr<MeshAsset>> testMeshes;
    GPUSceneData sceneData;
    vk::DescriptorSetLayout gpuSceneDataDescriptorLayout;

    // Texture data
    Image whiteImage;
    Image blackImage;
    Image greyImage;
    Image errorCheckerboardImage;
    vk::Sampler defaultSamplerLinear;
    vk::Sampler defaultSamplerNearest;
    vk::DescriptorSetLayout singleImageDescriptorLayout;

    MaterialInstance defaultData;
    pipelines::GLTFMetallicRoughness metalRoughMaterial;


    // ImGui
    vk::DescriptorPool imguiDescriptorPool;
    float rotationSpeed = 1.0f;
    float accumulatedRotation = 0.0f;
    std::chrono::high_resolution_clock::time_point lastFrameTime;
};

} // namespace graphics
