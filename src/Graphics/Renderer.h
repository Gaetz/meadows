#pragma once

#include "VulkanContext.h"
#include <vulkan/vulkan.hpp>
#include <vector>
#include <chrono>

#include <imgui.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_vulkan.h>

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
    
    vk::CommandPool commandPool;
    std::vector<vk::CommandBuffer> commandBuffers;

    std::vector<vk::Semaphore> imageAvailableSemaphores;
    std::vector<vk::Semaphore> renderFinishedSemaphores;
    std::vector<vk::Fence> inFlightFences;
    std::vector<vk::Fence> imagesInFlight;  // Track which fence is using each swapchain image


    vk::RenderPass renderPass;
    std::vector<vk::Framebuffer> framebuffers;
    class Pipeline* pipeline{ nullptr };

    class Buffer* vertexBuffer{ nullptr };
    uint32_t vertexCount = 0;

    class Buffer* indexBuffer{ nullptr };
    uint32_t indexCount = 0;

    std::vector<class Buffer*> uniformBuffers;
    vk::DescriptorPool descriptorPool;
    vk::DescriptorSetLayout descriptorSetLayout;
    std::vector<vk::DescriptorSet> descriptorSets;

    vk::PipelineLayout pipelineLayout;

    // ImGui
    vk::DescriptorPool imguiDescriptorPool;
    float rotationSpeed = 1.0f;
    float accumulatedRotation = 0.0f;
    std::chrono::high_resolution_clock::time_point lastFrameTime;

    uint32_t currentFrame = 0;
    static const int MAX_FRAMES_IN_FLIGHT = 2;
};
