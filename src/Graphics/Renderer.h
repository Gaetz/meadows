#pragma once

#include "../Defines.h"
#include "VulkanContext.h"
#include <vulkan/vulkan.hpp>
#include <vector>
#include <chrono>

#include "ComputeEffect.h"
#include "DeletionQueue.hpp"

namespace graphics {
    class Buffer;  // Forward declaration

    struct FrameData {
        vk::CommandPool commandPool;
        vk::CommandBuffer mainCommandBuffer;
        vk::Semaphore imageAvailableSemaphore;
        vk::Fence renderFence;
        DeletionQueue deletionQueue;
    };

class Renderer {
public:
    Renderer(VulkanContext* context);
    ~Renderer();

    void init();
    void cleanup();
    void draw();

private:
    void createCommandPoolAndBuffers();
    void createSyncObjects();
    void createRenderPass();
    void createFramebuffers();
    void createVertexBuffer();
    void createIndexBuffer();
    void createUniformBuffers();
    void createDescriptors();
    void createPipelines();
    void createBackgroundPipeline();
    void createTrianglePipeline();

    //void createDescriptorPool();
    //void createDescriptorSetLayout();
    //void createDescriptorSets();
    void updateUniformBuffer(uint32_t currentImage);

    void initImGui();
    void drawImGui(vk::CommandBuffer commandBuffer);

    void drawBackground(vk::CommandBuffer);
    void drawGeometry(vk::CommandBuffer);

    // Helper methods
    void copyBufferViaStaging(const void* data, vk::DeviceSize size, Buffer* dstBuffer);

    VulkanContext* context;

    int frameNumber {0};
    uint32_t currentFrame = 0;
    static const int FRAME_OVERLAP = 2;

    FrameData frames[FRAME_OVERLAP];
    FrameData& getCurrentFrame() { return frames[frameNumber % FRAME_OVERLAP]; };

    // One semaphore per swapchain image for proper synchronization
    std::vector<vk::Semaphore> renderFinishedSemaphores;

    vk::DescriptorSetLayout drawImageDescriptorLayout;
    vk::DescriptorSet drawImageDescriptors;
    vk::PipelineLayout pipelineLayout;

    // Compute effects
    std::vector<ComputeEffect> backgroundEffects;
    int currentBackgroundEffect{0};

    // Immediate submit structures
    vk::Fence immFence;
    vk::CommandPool immCommandPool;
    vk::CommandBuffer immCommandBuffer;
    void immediateSubmit(std::function<void(vk::CommandBuffer cmd)>&& function);

    // Graphics pipeline
    vk::PipelineLayout trianglePipelineLayout;
    vk::Pipeline trianglePipeline;


    //vk::CommandPool commandPool;
    //std::vector<vk::CommandBuffer> commandBuffers;

    //std::vector<vk::Semaphore> imageAvailableSemaphores;
    //std::vector<vk::Semaphore> renderFinishedSemaphores;
    //std::vector<vk::Fence> inFlightFences;
    //std::vector<vk::Fence> imagesInFlight;  // Track which fence is using each swapchain image

    //vk::RenderPass renderPass;
    //std::vector<vk::Framebuffer> framebuffers;
    //uptr<class Pipeline> pipeline;


    uptr<class Buffer> vertexBuffer;
    uint32_t vertexCount = 0;

    uptr<class Buffer> indexBuffer;
    uint32_t indexCount = 0;

    std::vector<uptr<class Buffer>> uniformBuffers;


    // ImGui
    vk::DescriptorPool imguiDescriptorPool;
    float rotationSpeed = 1.0f;
    float accumulatedRotation = 0.0f;
    std::chrono::high_resolution_clock::time_point lastFrameTime;
};

} // namespace graphics
