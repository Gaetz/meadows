#include "Renderer.h"
#include "Swapchain.h"
#include "Pipeline.h"
#include "Buffer.h"
#include <array>
#include <chrono>
#include <cassert>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_vulkan.h>

namespace graphics {

Renderer::Renderer(VulkanContext* context) : context(context) {
    lastFrameTime = std::chrono::high_resolution_clock::now();
}

Renderer::~Renderer() {
    cleanup();
}

void Renderer::init() {
    createRenderPass();
    createDescriptorSetLayout();
    createFramebuffers();
    createPipeline();
    createCommandPool();
    createVertexBuffer();
    createIndexBuffer();
    createUniformBuffers();
    createDescriptorPool();
    createDescriptorSets();
    createCommandBuffers();
    createSyncObjects();
    
    initImGui();
}

void Renderer::cleanup() {
    if (!commandPool) return; // Already cleaned up or never initialized

    vk::Device device = context->getDevice();

    device.waitIdle();

    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    device.destroyDescriptorPool(imguiDescriptorPool);
    device.destroyDescriptorPool(descriptorPool);
    device.destroyDescriptorSetLayout(descriptorSetLayout);
    device.destroyPipelineLayout(pipelineLayout);

    // Destroy sync objects
    for (auto semaphore : imageAvailableSemaphores) {
        device.destroySemaphore(semaphore);
    }
    for (auto semaphore : renderFinishedSemaphores) {
        device.destroySemaphore(semaphore);
    }
    for (auto fence : inFlightFences) {
        device.destroyFence(fence);
    }

    // unique_ptr automatically deletes when cleared
    uniformBuffers.clear();
    indexBuffer.reset();
    vertexBuffer.reset();
    pipeline.reset();

    for (auto framebuffer : framebuffers) {
        device.destroyFramebuffer(framebuffer);
    }
    framebuffers.clear();

    device.destroyRenderPass(renderPass);
    device.destroyCommandPool(commandPool);
}

void Renderer::createCommandPool() {
    QueueFamilyIndices queueFamilyIndices = context->findQueueFamilies(context->getPhysicalDevice());

    vk::CommandPoolCreateInfo poolInfo(
        vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
        queueFamilyIndices.graphicsFamily.value()
    );

    commandPool = context->getDevice().createCommandPool(poolInfo);
}

void Renderer::createCommandBuffers() {
    commandBuffers.resize(MAX_FRAMES_IN_FLIGHT);

    vk::CommandBufferAllocateInfo allocInfo(
        commandPool,
        vk::CommandBufferLevel::ePrimary,
        (uint32_t)commandBuffers.size()
    );

    commandBuffers = context->getDevice().allocateCommandBuffers(allocInfo);
}

void Renderer::createRenderPass() {
    vk::AttachmentDescription colorAttachment(
        {},
        context->getSwapchain()->getImageFormat(),
        vk::SampleCountFlagBits::e1,
        vk::AttachmentLoadOp::eClear,
        vk::AttachmentStoreOp::eStore,
        vk::AttachmentLoadOp::eDontCare,
        vk::AttachmentStoreOp::eDontCare,
        vk::ImageLayout::eUndefined,
        vk::ImageLayout::ePresentSrcKHR
    );

    vk::AttachmentReference colorAttachmentRef(0, vk::ImageLayout::eColorAttachmentOptimal);

    vk::SubpassDescription subpass(
        {},
        vk::PipelineBindPoint::eGraphics,
        0, nullptr,
        1, &colorAttachmentRef
    );

    vk::SubpassDependency dependency(
        VK_SUBPASS_EXTERNAL,
        0,
        vk::PipelineStageFlagBits::eColorAttachmentOutput,
        vk::PipelineStageFlagBits::eColorAttachmentOutput,
        {},
        vk::AccessFlagBits::eColorAttachmentWrite
    );

    vk::RenderPassCreateInfo renderPassInfo(
        {},
        1, &colorAttachment,
        1, &subpass,
        1, &dependency
    );

    renderPass = context->getDevice().createRenderPass(renderPassInfo);
}

void Renderer::createFramebuffers() {
    const auto& imageViews = context->getSwapchain()->getImageViews();
    vk::Extent2D extent = context->getSwapchain()->getExtent();

    framebuffers.resize(imageViews.size());

    for (size_t i = 0; i < imageViews.size(); i++) {
        vk::ImageView attachments[] = {
            imageViews[i]
        };

        vk::FramebufferCreateInfo framebufferInfo(
            {},
            renderPass,
            1, attachments,
            extent.width,
            extent.height,
            1
        );

        framebuffers[i] = context->getDevice().createFramebuffer(framebufferInfo);
    }
}

void Renderer::createPipeline() {
    PipelineConfigInfo pipelineConfig{};
    Pipeline::defaultPipelineConfigInfo(pipelineConfig);
    pipelineConfig.renderPass = renderPass;
    
    vk::PipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &descriptorSetLayout;
    
    pipelineLayout = context->getDevice().createPipelineLayout(pipelineLayoutInfo);
    pipelineConfig.pipelineLayout = pipelineLayout;

    pipeline = std::make_unique<Pipeline>(
        context,
        "shaders/shader.vert.spv",
        "shaders/shader.frag.spv",
        pipelineConfig
    );
}



void Renderer::createVertexBuffer() {
    std::vector<Vertex> vertices = {
        // Front face
        {{-0.5f, -0.5f, 0.5f}, {1.0f, 0.0f, 0.0f}}, // 0
        {{0.5f, -0.5f, 0.5f}, {0.0f, 1.0f, 0.0f}},  // 1
        {{0.5f, 0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}},   // 2
        {{-0.5f, 0.5f, 0.5f}, {1.0f, 1.0f, 1.0f}},  // 3
        // Back face
        {{-0.5f, -0.5f, -0.5f}, {1.0f, 0.0f, 1.0f}}, // 4
        {{0.5f, -0.5f, -0.5f}, {0.0f, 1.0f, 1.0f}},  // 5
        {{0.5f, 0.5f, -0.5f}, {1.0f, 1.0f, 0.0f}},   // 6
        {{-0.5f, 0.5f, -0.5f}, {0.0f, 0.0f, 0.0f}}   // 7
    };

    vertexCount = static_cast<uint32_t>(vertices.size());
    vk::DeviceSize bufferSize = sizeof(vertices[0]) * vertices.size();

    // Create vertex buffer
    vertexBuffer = std::make_unique<Buffer>(
        context,
        bufferSize,
        vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferDst,
        VMA_MEMORY_USAGE_GPU_ONLY
    );

    copyBufferViaStaging(vertices.data(), bufferSize, vertexBuffer.get());
}

void Renderer::copyBufferViaStaging(const void* data, vk::DeviceSize size, Buffer* dstBuffer) {
    // Staging buffer
    Buffer stagingBuffer(
        context,
        size,
        vk::BufferUsageFlagBits::eTransferSrc,
        VMA_MEMORY_USAGE_CPU_ONLY
    );

    stagingBuffer.write(const_cast<void*>(data), size);

    // Copy from staging to destination buffer  
    vk::CommandBufferAllocateInfo allocInfo(
        commandPool,
        vk::CommandBufferLevel::ePrimary,
        1
    );

    vk::CommandBuffer commandBuffer = context->getDevice().allocateCommandBuffers(allocInfo)[0];

    vk::CommandBufferBeginInfo beginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
    commandBuffer.begin(beginInfo);
    
    vk::BufferCopy copyRegion(0, 0, size);
    commandBuffer.copyBuffer(stagingBuffer.getBuffer(), dstBuffer->getBuffer(), 1, &copyRegion);

    commandBuffer.end();

    vk::SubmitInfo submitInfo;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    (void)context->getGraphicsQueue().submit(1, &submitInfo, nullptr);
    context->getGraphicsQueue().waitIdle();

    context->getDevice().freeCommandBuffers(commandPool, 1, &commandBuffer);
}

void Renderer::createIndexBuffer() {
    std::vector<uint16_t> indices = {
        // Front
        0, 1, 2, 2, 3, 0,
        // Back
        5, 4, 7, 7, 6, 5,
        // Left
        4, 0, 3, 3, 7, 4,
        // Right
        1, 5, 6, 6, 2, 1,
        // Top
        4, 5, 1, 1, 0, 4,
        // Bottom
        3, 2, 6, 6, 7, 3
    };

    indexCount = static_cast<uint32_t>(indices.size());
    vk::DeviceSize bufferSize = sizeof(indices[0]) * indices.size();

    indexBuffer = std::make_unique<Buffer>(
        context,
        bufferSize,
        vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eTransferDst,
        VMA_MEMORY_USAGE_GPU_ONLY
    );

    copyBufferViaStaging(indices.data(), bufferSize, indexBuffer.get());
}

void Renderer::createDescriptorSetLayout() {
    vk::DescriptorSetLayoutBinding uboLayoutBinding(
        0,
        vk::DescriptorType::eUniformBuffer,
        1,
        vk::ShaderStageFlagBits::eVertex,
        nullptr
    );

    vk::DescriptorSetLayoutCreateInfo layoutInfo(
        {},
        1, &uboLayoutBinding
    );

    descriptorSetLayout = context->getDevice().createDescriptorSetLayout(layoutInfo);
}

void Renderer::createUniformBuffers() {
    vk::DeviceSize bufferSize = sizeof(UniformBufferObject);

    uniformBuffers.resize(MAX_FRAMES_IN_FLIGHT);

    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        uniformBuffers[i] = std::make_unique<Buffer>(
            context,
            bufferSize,
            vk::BufferUsageFlagBits::eUniformBuffer,
            VMA_MEMORY_USAGE_CPU_TO_GPU
        );
    }
}

void Renderer::createDescriptorPool() {
    vk::DescriptorPoolSize poolSize(
        vk::DescriptorType::eUniformBuffer,
        static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT)
    );

    vk::DescriptorPoolCreateInfo poolInfo(
        {},
        static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT),
        1, &poolSize
    );

    descriptorPool = context->getDevice().createDescriptorPool(poolInfo);
}

void Renderer::createDescriptorSets() {
    std::vector<vk::DescriptorSetLayout> layouts(MAX_FRAMES_IN_FLIGHT, descriptorSetLayout);
    vk::DescriptorSetAllocateInfo allocInfo(
        descriptorPool,
        static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT),
        layouts.data()
    );

    descriptorSets = context->getDevice().allocateDescriptorSets(allocInfo);

    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        vk::DescriptorBufferInfo bufferInfo(
            uniformBuffers[i]->getBuffer(),
            0,
            sizeof(UniformBufferObject)
        );

        vk::WriteDescriptorSet descriptorWrite(
            descriptorSets[i],
            0,
            0,
            1,
            vk::DescriptorType::eUniformBuffer,
            nullptr,
            &bufferInfo,
            nullptr
        );

        context->getDevice().updateDescriptorSets(1, &descriptorWrite, 0, nullptr);
    }
}

void Renderer::updateUniformBuffer(uint32_t currentImage) {
    auto currentTime = std::chrono::high_resolution_clock::now();
    float dt = std::chrono::duration<float, std::chrono::seconds::period>(currentTime - lastFrameTime).count();
    lastFrameTime = currentTime;

    accumulatedRotation += dt * rotationSpeed;

    UniformBufferObject ubo{};
    ubo.model = glm::rotate(glm::mat4(1.0f), accumulatedRotation * glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    ubo.model = glm::rotate(ubo.model, accumulatedRotation * glm::radians(45.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    ubo.view = glm::lookAt(glm::vec3(2.0f, 2.0f, 2.0f), glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    ubo.proj = glm::perspective(glm::radians(45.0f), context->getSwapchain()->getExtent().width / (float)context->getSwapchain()->getExtent().height, 0.1f, 10.0f);
    ubo.proj[1][1] *= -1;

    uniformBuffers[currentImage]->write(&ubo, sizeof(ubo));
}

void Renderer::createSyncObjects() {
    // Get swapchain image count
    size_t imageCount = context->getSwapchain()->getImageViews().size();
    
    // Allocate semaphores per swapchain image (not per frame)
    imageAvailableSemaphores.resize(imageCount);
    renderFinishedSemaphores.resize(imageCount);
    
    // Fences are still per frame
    inFlightFences.resize(MAX_FRAMES_IN_FLIGHT);
    imagesInFlight.resize(imageCount, vk::Fence());

    vk::SemaphoreCreateInfo semaphoreInfo;
    vk::FenceCreateInfo fenceInfo(vk::FenceCreateFlagBits::eSignaled);

    vk::Device device = context->getDevice();

    // Create semaphores for each swapchain image
    for (size_t i = 0; i < imageCount; i++) {
        imageAvailableSemaphores[i] = device.createSemaphore(semaphoreInfo);
        renderFinishedSemaphores[i] = device.createSemaphore(semaphoreInfo);
    }
    
    // Create fences for each frame in flight
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        inFlightFences[i] = device.createFence(fenceInfo);
    }
}

void Renderer::draw() {
    vk::Device device = context->getDevice();
    vk::SwapchainKHR swapchain = context->getSwapchain()->getSwapchain();

    // Wait for previous frame
    vk::Result fenceResult = device.waitForFences(1, &inFlightFences[currentFrame], VK_TRUE, UINT64_MAX);
    assert(fenceResult == vk::Result::eSuccess && "failed to wait for fence!");
    
    uint32_t imageIndex;
    // We still need to use currentFrame to avoid using uninitialized imageIndex
    // But the real fix is using imageAvailableSemaphores indexed by a cyclic counter
    static uint32_t semaphoreIndex = 0;
    vk::Result result = device.acquireNextImageKHR(swapchain, UINT64_MAX, imageAvailableSemaphores[semaphoreIndex % imageAvailableSemaphores.size()], nullptr, &imageIndex);
    semaphoreIndex++;
    
    if (result == vk::Result::eErrorOutOfDateKHR) {
        // Recreate swapchain (TODO)
        return;
    }
    assert((result == vk::Result::eSuccess || result == vk::Result::eSuboptimalKHR) && "failed to acquire swap chain image!");

    // Check if a previous frame is using this image (wait on its fence)
    if (imagesInFlight[imageIndex]) {
        (void)device.waitForFences(1, &imagesInFlight[imageIndex], VK_TRUE, UINT64_MAX);
    }
    
    // Mark the image as now being in use by this frame
    imagesInFlight[imageIndex] = inFlightFences[currentFrame];

    (void)device.resetFences(1, &inFlightFences[currentFrame]);

    commandBuffers[currentFrame].reset();

    updateUniformBuffer(currentFrame);

    // Record command buffer
    vk::CommandBufferBeginInfo beginInfo;
    commandBuffers[currentFrame].begin(beginInfo);

    vk::RenderPassBeginInfo renderPassInfo(
        renderPass,
        framebuffers[imageIndex],
        vk::Rect2D({0, 0}, context->getSwapchain()->getExtent())
    );

    std::array<vk::ClearValue, 1> clearValues{};
    clearValues[0].color = vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f});

    renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    renderPassInfo.pClearValues = clearValues.data();

    commandBuffers[currentFrame].beginRenderPass(renderPassInfo, vk::SubpassContents::eInline);

    vk::Viewport viewport(
        0.0f, 0.0f,
        (float)context->getSwapchain()->getExtent().width,
        (float)context->getSwapchain()->getExtent().height,
        0.0f, 1.0f
    );
    vk::Rect2D scissor({0, 0}, context->getSwapchain()->getExtent());
    
    commandBuffers[currentFrame].setViewport(0, 1, &viewport);
    commandBuffers[currentFrame].setScissor(0, 1, &scissor);

    pipeline->bind(commandBuffers[currentFrame]);

    commandBuffers[currentFrame].bindDescriptorSets(
        vk::PipelineBindPoint::eGraphics,
        pipelineLayout,
        0,
        1,
        &descriptorSets[currentFrame],
        0,
        nullptr
    );

    vk::Buffer vertexBuffers[] = {vertexBuffer->getBuffer()};
    vk::DeviceSize offsets[] = {0};
    commandBuffers[currentFrame].bindVertexBuffers(0, 1, vertexBuffers, offsets);

    commandBuffers[currentFrame].bindIndexBuffer(indexBuffer->getBuffer(), 0, vk::IndexType::eUint16);

    commandBuffers[currentFrame].drawIndexed(indexCount, 1, 0, 0, 0);

    // Draw ImGui
    drawImGui(commandBuffers[currentFrame]);

    commandBuffers[currentFrame].endRenderPass();
    commandBuffers[currentFrame].end();

    vk::SubmitInfo submitInfo;
    vk::Semaphore waitSemaphores[] = { imageAvailableSemaphores[imageIndex] };
    vk::PipelineStageFlags waitStages[] = { vk::PipelineStageFlagBits::eColorAttachmentOutput };
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;

    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffers[currentFrame];

    vk::Semaphore signalSemaphores[] = { renderFinishedSemaphores[imageIndex] };
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSemaphores;

    vk::Result submitResult = context->getGraphicsQueue().submit(1, &submitInfo, inFlightFences[currentFrame]);
    assert(submitResult == vk::Result::eSuccess && "failed to submit draw command buffer!");

    vk::PresentInfoKHR presentInfo;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = signalSemaphores;

    vk::SwapchainKHR swapchains[] = { swapchain };
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = swapchains;
    presentInfo.pImageIndices = &imageIndex;

    result = context->getPresentQueue().presentKHR(&presentInfo);

    if (result == vk::Result::eErrorOutOfDateKHR || result == vk::Result::eSuboptimalKHR) {
        // Recreate swapchain (TODO)
    } else {
        assert(result == vk::Result::eSuccess && "failed to present swap chain image!");
    }

    currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;

}

void Renderer::initImGui() {
    // 1: Create descriptor pool for ImGui
    vk::DescriptorPoolSize pool_sizes[] =
    {
        { vk::DescriptorType::eSampler, 1000 },
        { vk::DescriptorType::eCombinedImageSampler, 1000 },
        { vk::DescriptorType::eSampledImage, 1000 },
        { vk::DescriptorType::eStorageImage, 1000 },
        { vk::DescriptorType::eUniformTexelBuffer, 1000 },
        { vk::DescriptorType::eStorageTexelBuffer, 1000 },
        { vk::DescriptorType::eUniformBuffer, 1000 },
        { vk::DescriptorType::eStorageBuffer, 1000 },
        { vk::DescriptorType::eUniformBufferDynamic, 1000 },
        { vk::DescriptorType::eStorageBufferDynamic, 1000 },
        { vk::DescriptorType::eInputAttachment, 1000 }
    };

    vk::DescriptorPoolCreateInfo pool_info = {};
    pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    pool_info.maxSets = 1000;
    pool_info.poolSizeCount = std::size(pool_sizes);
    pool_info.pPoolSizes = pool_sizes;

    imguiDescriptorPool = context->getDevice().createDescriptorPool(pool_info);

    // 2: Initialize ImGui library
    ImGui::CreateContext();

    // 3: Initialize ImGui for SDL3
    ImGui_ImplSDL3_InitForVulkan(context->getWindow());

    // 4: Initialize ImGui for Vulkan
    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.Instance = (VkInstance)context->getInstance();
    init_info.PhysicalDevice = (VkPhysicalDevice)context->getPhysicalDevice();
    init_info.Device = (VkDevice)context->getDevice();
    init_info.Queue = (VkQueue)context->getGraphicsQueue();
    init_info.DescriptorPool = (VkDescriptorPool)imguiDescriptorPool;
    init_info.MinImageCount = 3;
    init_info.ImageCount = 3;
    // init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    // init_info.RenderPass = (VkRenderPass)renderPass; // Deprecated
    init_info.PipelineInfoMain.RenderPass = (VkRenderPass)renderPass;
    init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init_info.PipelineInfoMain.Subpass = 0;

    ImGui_ImplVulkan_Init(&init_info);

    // Upload fonts
    // ImGui_ImplVulkan_CreateFontsTexture(); // Removed in newer ImGui versions
}

void Renderer::drawImGui(vk::CommandBuffer commandBuffer) {
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    ImGui::Begin("Vulkan Engine");
    ImGui::Text("Hello, World!");
    ImGui::SliderFloat("Rotation Speed", &rotationSpeed, 0.0f, 5.0f);
    ImGui::End();

    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);
}

} // namespace graphics
