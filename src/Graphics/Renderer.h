#pragma once

#include "../Defines.h"
#include "VulkanContext.h"
#include <vulkan/vulkan.hpp>
#include <vector>
#include <chrono>

namespace graphics {

    struct FrameData {
        vk::CommandPool commandPool;
        vk::CommandBuffer mainCommandBuffer;
        vk::Semaphore imageAvailableSemaphore, renderFinishedSemaphore;
        vk::Fence renderFence;
    };

class Buffer;  // Forward declaration

class Renderer {
public:
    Renderer(VulkanContext* context);
    ~Renderer();

    void init();
    void cleanup();
    void draw();

private:
    void createCommandPool();
    void createCommandBuffers();
    void createSyncObjects();
    void createRenderPass();
    void createFramebuffers();
    void createPipeline();
    void createVertexBuffer();
    void createIndexBuffer();
    void createUniformBuffers();
    void createDescriptorPool();
    void createDescriptorSetLayout();
    void createDescriptorSets();
    void updateUniformBuffer(uint32_t currentImage);

    void initImGui();
    void drawImGui(vk::CommandBuffer commandBuffer);
    
    // Helper methods
    void copyBufferViaStaging(const void* data, vk::DeviceSize size, Buffer* dstBuffer);

    VulkanContext* context;

    int frameNumber {0};
    uint32_t currentFrame = 0;
    static const int FRAME_OVERLAP = 2;

    FrameData frames[FRAME_OVERLAP];
    FrameData& getCurrentFrame() { return frames[frameNumber % FRAME_OVERLAP]; };

    //vk::CommandPool commandPool;
    //std::vector<vk::CommandBuffer> commandBuffers;

    //std::vector<vk::Semaphore> imageAvailableSemaphores;
    //std::vector<vk::Semaphore> renderFinishedSemaphores;
    //std::vector<vk::Fence> inFlightFences;
    //std::vector<vk::Fence> imagesInFlight;  // Track which fence is using each swapchain image


    vk::RenderPass renderPass;
    std::vector<vk::Framebuffer> framebuffers;
    uptr<class Pipeline> pipeline;

    uptr<class Buffer> vertexBuffer;
    uint32_t vertexCount = 0;

    uptr<class Buffer> indexBuffer;
    uint32_t indexCount = 0;

    std::vector<uptr<class Buffer>> uniformBuffers;
    vk::DescriptorPool descriptorPool;
    vk::DescriptorSetLayout descriptorSetLayout;
    std::vector<vk::DescriptorSet> descriptorSets;

    vk::PipelineLayout pipelineLayout;

    // ImGui
    vk::DescriptorPool imguiDescriptorPool;
    float rotationSpeed = 1.0f;
    float accumulatedRotation = 0.0f;
    std::chrono::high_resolution_clock::time_point lastFrameTime;
};

} // namespace graphics
