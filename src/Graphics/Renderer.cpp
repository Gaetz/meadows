#include "Renderer.h"
#include "Swapchain.h"
#include "Pipeline.h"
#include "Buffer.h"
#include <array>
#include <chrono>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_vulkan.h>

#include "DescriptorLayoutBuilder.hpp"
#include "PipelineCompute.h"
#include "Utils.hpp"
#include "VulkanInit.hpp"

namespace graphics {

Renderer::Renderer(VulkanContext* context) : context(context) {
    lastFrameTime = std::chrono::high_resolution_clock::now();
}

Renderer::~Renderer() {
    cleanup();
}

void Renderer::init() {
    //createRenderPass();
    //createDescriptorSetLayout();
    //createFramebuffers();
    //createPipeline();
    createCommandPoolAndBuffers();
    //createVertexBuffer();
    //createIndexBuffer();
    //createUniformBuffers();
    //createDescriptorPool();
    //createDescriptorSets();
    //createCommandBuffers();
    createSyncObjects();
    createDescriptors();
    createPipelines();
    
    initImGui();
}

void Renderer::cleanup() {
    if (!frames[0].commandPool) return; // Already cleaned up or never initialized

    vk::Device device = context->getDevice();

    device.waitIdle();

    // Cleanup ImGui
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    device.destroyDescriptorPool(imguiDescriptorPool);

    for (int i = 0; i < FRAME_OVERLAP; i++)
    {
        device.destroyCommandPool(frames[i].commandPool);
        device.destroySemaphore(frames[i].imageAvailableSemaphore);
        device.destroyFence(frames[i].renderFence);
        frames[i].deletionQueue.flush();
    }

    for (auto& semaphore : renderFinishedSemaphores) {
        device.destroySemaphore(semaphore);
    }
    renderFinishedSemaphores.clear();

    context->flushMainDeletionQueue();
}

void Renderer::createCommandPoolAndBuffers() {
    const QueueFamilyIndices queueFamilyIndices = context->findQueueFamilies(context->getPhysicalDevice());

    const auto poolInfo = graphics::commandPoolCreateInfo(
        queueFamilyIndices.graphicsFamily.value(),
        vk::CommandPoolCreateFlagBits::eResetCommandBuffer
    );

    // Frame-specific command pools and buffers
    for (int i = 0; i < FRAME_OVERLAP; i++)
    {
        frames[i].commandPool = context->getDevice().createCommandPool(poolInfo);

        vk::CommandBufferAllocateInfo cmdAllocInfo = graphics::commandBufferAllocateInfo(frames[i].commandPool, 1);
        frames[i].mainCommandBuffer = context->getDevice().allocateCommandBuffers(cmdAllocInfo)[0];
    }

    // Immediate command pool
    immCommandPool = context->getDevice().createCommandPool(poolInfo);
    vk::CommandBufferAllocateInfo cmdAllocInfo = graphics::commandBufferAllocateInfo(immCommandPool, 1);
    immCommandBuffer = context->getDevice().allocateCommandBuffers(cmdAllocInfo)[0];
    context->addToMainDeletionQueue([this]() {
        context->getDevice().destroyCommandPool(immCommandPool);
    });
}

void Renderer::createDescriptors() {
    // Make the descriptor set layout for our compute draw
    DescriptorLayoutBuilder layoutBuilder;
    layoutBuilder.addBinding(0, vk::DescriptorType::eStorageImage);
    drawImageDescriptorLayout = layoutBuilder.build(context->getDevice(), vk::ShaderStageFlagBits::eCompute);

    // Allocate a descriptor set for our draw image
    drawImageDescriptors = context->getGlobalDescriptorAllocator()->allocate(drawImageDescriptorLayout);

    vk::DescriptorImageInfo imageInfo = {};
    imageInfo.imageLayout = vk::ImageLayout::eGeneral;
    imageInfo.imageView = context->getDrawImage().imageView;

    vk::WriteDescriptorSet descriptorWrite = {};
    descriptorWrite.dstBinding = 0;
    descriptorWrite.dstSet = drawImageDescriptors;
    descriptorWrite.descriptorCount = 1;
    descriptorWrite.descriptorType = vk::DescriptorType::eStorageImage;
    descriptorWrite.pImageInfo = &imageInfo;

    context->getDevice().updateDescriptorSets(1, &descriptorWrite, 0, nullptr);

    // Cleanup
    context->addToMainDeletionQueue([&]() {
        context->getDevice().destroyDescriptorSetLayout(drawImageDescriptorLayout);
    });
}

void Renderer::createRenderPass() {
    /*
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
    */
}

void Renderer::createFramebuffers() {
    /*
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
    */
}

void Renderer::createPipelines() {
    createBackgroundPipeline();
}

void Renderer::createBackgroundPipeline() {

    vk::PipelineLayoutCreateInfo computeLayout{};
    computeLayout.setLayoutCount = 1;
    computeLayout.pSetLayouts = &drawImageDescriptorLayout;

    /*
    // Basic compute pipeline layout
    pipelineLayout = context->getDevice().createPipelineLayout(computeLayout);
    computePipeline = std::make_unique<PipelineCompute>(context, "shaders/gradient.comp.spv", pipelineLayout);
    */


    // Pipeline layout with push constants
    vk::PushConstantRange pushConstant {};
    pushConstant.offset = 0;
    pushConstant.size = sizeof(ComputePushConstants);
    pushConstant.stageFlags = vk::ShaderStageFlagBits::eCompute;

    computeLayout.pushConstantRangeCount = 1;
    computeLayout.pPushConstantRanges = &pushConstant;
    pipelineLayout = context->getDevice().createPipelineLayout(computeLayout);

    /*
    // Simple compute pipeline with push constants
    computePipeline = std::make_unique<PipelineCompute>(context, "shaders/gradientCustom.comp.spv", pipelineLayout);
    */

    ComputeEffect gradient {};
    gradient = ComputeEffect(context, "shaders/gradientCustom.comp.spv", pipelineLayout);





/*
    PipelineConfigInfo pipelineConfig{};
    Pipeline::defaultPipelineConfigInfo(pipelineConfig);
    pipelineConfig.renderPass = renderPass;



    pipelineConfig.pipelineLayout = pipelineLayout;

    pipeline = std::make_unique<Pipeline>(
        context,
        "shaders/shader.vert.spv",
        "shaders/shader.frag.spv",
        pipelineConfig
    );
    */
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
    /*
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
    */
}

void Renderer::immediateSubmit(std::function<void(vk::CommandBuffer cmd)> &&function) {
    context->getDevice().resetFences(immFence);
    immCommandBuffer.reset();

    vk::CommandBufferBeginInfo beginInfo = graphics::commandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
    immCommandBuffer.begin(beginInfo);

    function(immCommandBuffer);

    immCommandBuffer.end();

    vk::CommandBufferSubmitInfo submitInfo = graphics::commandBufferSubmitInfo(immCommandBuffer);
    vk::SubmitInfo2 submit = graphics::submitInfo(submitInfo, nullptr, nullptr);
    const auto res = context->getGraphicsQueue().submit2(1, &submit, immFence);
    const auto res2 = context->getDevice().waitForFences(1, &immFence, true, UINT64_MAX);
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

void Renderer::createUniformBuffers() {
    vk::DeviceSize bufferSize = sizeof(UniformBufferObject);

    uniformBuffers.resize(FRAME_OVERLAP);

    for (size_t i = 0; i < FRAME_OVERLAP; i++) {
        uniformBuffers[i] = std::make_unique<Buffer>(
            context,
            bufferSize,
            vk::BufferUsageFlagBits::eUniformBuffer,
            VMA_MEMORY_USAGE_CPU_TO_GPU
        );
    }
}

    /*
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

void Renderer::createDescriptorPool() {
    vk::DescriptorPoolSize poolSize(
        vk::DescriptorType::eUniformBuffer,
        static_cast<uint32_t>(FRAME_OVERLAP)
    );

    vk::DescriptorPoolCreateInfo poolInfo(
        {},
        static_cast<uint32_t>(FRAME_OVERLAP),
        1, &poolSize
    );

    descriptorPool = context->getDevice().createDescriptorPool(poolInfo);
}

void Renderer::createDescriptorSets() {
    std::vector<vk::DescriptorSetLayout> layouts(FRAME_OVERLAP, descriptorSetLayout);
    vk::DescriptorSetAllocateInfo allocInfo(
        descriptorPool,
        static_cast<uint32_t>(FRAME_OVERLAP),
        layouts.data()
    );

    descriptorSets = context->getDevice().allocateDescriptorSets(allocInfo);

    for (size_t i = 0; i < FRAME_OVERLAP; i++) {
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
    */

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
    vk::SemaphoreCreateInfo semaphoreInfo = graphics::semaphoreCreateInfo();
    vk::FenceCreateInfo fenceInfo = graphics::fenceCreateInfo(vk::FenceCreateFlagBits::eSignaled);

    vk::Device device = context->getDevice();

    // Create command pool sync objects for frames in flight
    for (size_t i = 0; i < FRAME_OVERLAP; i++) {
        frames[i].renderFence = device.createFence(fenceInfo);
        frames[i].imageAvailableSemaphore = device.createSemaphore(semaphoreInfo);
    }

    // Create one render finished semaphore per swapchain image
    size_t imageCount = context->getSwapchain()->getImages().size();
    renderFinishedSemaphores.resize(imageCount);
    for (size_t i = 0; i < imageCount; i++) {
        renderFinishedSemaphores[i] = device.createSemaphore(semaphoreInfo);
    }

    // Create a fence for immediate submits
    immFence = device.createFence(graphics::fenceCreateInfo());
    context->addToMainDeletionQueue([this]() {
        context->getDevice().destroyFence(immFence);
    });
}

void Renderer::drawBackground(vk::CommandBuffer command) {
    // Clear color
    /*
    float flash = std::abs(std::sin(frameNumber / 120.f));
    vk::ClearColorValue clearValue = vk::ClearColorValue { 0.0f, 0.0f, flash, 1.0f };

    vk::ImageSubresourceRange clearRange = graphics::imageSubresourceRange(vk::ImageAspectFlagBits::eColor);
    command.clearColorImage(context->getDrawImage().image,
        vk::ImageLayout::eGeneral, &clearValue,1, &clearRange);
    */


    // Bind compute pipeline and descriptor sets
    constexpr auto bindPoint = vk::PipelineBindPoint::eCompute;
    command.bindPipeline(bindPoint, computePipeline->get());
    command.bindDescriptorSets(bindPoint, pipelineLayout, 0, 1, &drawImageDescriptors, 0, nullptr);

    ComputePushConstants pushConstants{};
    pushConstants.data1 = Vec4 { 1, 0, 0, 1 };
    pushConstants.data2 = Vec4 { 0, 0, 1, 1 };
    command.pushConstants<ComputePushConstants>(pipelineLayout, vk::ShaderStageFlagBits::eCompute, 0, pushConstants);

    command.dispatch(std::ceil(context->getDrawImage().imageExtent.width / 16.0f),
                     std::ceil(context->getDrawImage().imageExtent.height / 16.0f),
                     1);
}


void Renderer::draw() {
    const vk::Device device = context->getDevice();
    FrameData& currentFrameData = getCurrentFrame();

    // Wait for previous frame
    vk::Result fenceResult = device.waitForFences(1, &currentFrameData.renderFence, true, 1000000000);
    currentFrameData.deletionQueue.flush();
    const auto res = device.resetFences(1, &currentFrameData.renderFence);

    // Request image from the swapchain
    u32 imageIndex;
    vk::Result result = device.acquireNextImageKHR(*context->getSwapchain()->getSwapchain(), 1000000000, currentFrameData.imageAvailableSemaphore, nullptr, &imageIndex);

    //acquireSemaphoreIndex++;

    const vk::CommandBuffer command = currentFrameData.mainCommandBuffer;
    command.reset();

    // TODO put this again with the right current frame
    //updateUniformBuffer(currentFrame);

    // Record command buffer
    vk::CommandBufferBeginInfo beginInfo = graphics::commandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);

    vk::Extent2D drawExtent;
    drawExtent.width = context->getDrawImage().imageExtent.width;
    drawExtent.height = context->getDrawImage().imageExtent.height;
    AllocatedImage& drawImage = context->getDrawImage();
    command.begin(beginInfo);

    // Make the swapchain image into writeable mode before rendering
    graphics::transitionImage(command, drawImage.image,
        vk::ImageLayout::eUndefined,vk::ImageLayout::eGeneral);

    drawBackground(command);

	// Transition the draw image and the swapchain image into their correct transfer layouts
    graphics::transitionImage(command, drawImage.image,vk::ImageLayout::eGeneral, vk::ImageLayout::eTransferSrcOptimal);
    graphics::transitionImage(command, context->getSwapchain()->getImages()[imageIndex],vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal);

    // Execute a copy from the draw image into the swapchain
    graphics::copyImageToImage(command, drawImage.image, context->getSwapchain()->getImages()[imageIndex], drawExtent, context->getSwapchain()->getExtent());

    // Transition swapchain image to color attachment for ImGui rendering
    graphics::transitionImage(command, context->getSwapchain()->getImages()[imageIndex], vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eColorAttachmentOptimal);

    // Render ImGui on top of the swapchain image
    vk::RenderingAttachmentInfo colorAttachment = graphics::attachmentInfo(context->getSwapchain()->getImageViews()[imageIndex], nullptr, vk::ImageLayout::eColorAttachmentOptimal);
    vk::RenderingInfo renderInfo = graphics::renderingInfo(vk::Rect2D({0, 0}, context->getSwapchain()->getExtent()), &colorAttachment);

    command.beginRendering(renderInfo);
    drawImGui(command);
    command.endRendering();

    // Set swapchain image layout to Present so we can show it on the screen
    graphics::transitionImage(command, context->getSwapchain()->getImages()[imageIndex], vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::ePresentSrcKHR);

    // Finalize the command buffer (we can no longer add commands, but it can now be executed)
    command.end();

    /* Prepare the submission to the queue.
     * We want to wait on the imageAvailableSemaphore, as that semaphore is signaled when the swapchain is ready
     * We will signal the renderFinishedSemaphore, to signal that rendering has finished
     */

    const vk::CommandBufferSubmitInfo commandInfo = graphics::commandBufferSubmitInfo(command);
    vk::SemaphoreSubmitInfo waitInfo = graphics::semaphoreSubmitInfo(currentFrameData.imageAvailableSemaphore, vk::PipelineStageFlagBits2::eColorAttachmentOutput);
    vk::SemaphoreSubmitInfo signalInfo = graphics::semaphoreSubmitInfo(renderFinishedSemaphores[imageIndex], vk::PipelineStageFlagBits2::eAllGraphics);
    const vk::SubmitInfo2 submit = graphics::submitInfo(commandInfo, &signalInfo, &waitInfo);

    /* Submit command buffer to the queue and execute it.
     * renderFence will now block until the graphic commands finish execution
    */
    vk::Result submitResult = context->getGraphicsQueue().submit2(1, &submit, currentFrameData.renderFence);

    /* Prepare present
     * This will put the image we just rendered to into the visible window.
     * we want to wait on the renderFinishedSemaphore for that, as its necessary that
     * drawing commands have finished before the image is displayed to the user.
    */
    vk::PresentInfoKHR presentInfo = {};
    presentInfo.pNext = nullptr;
    presentInfo.pSwapchains = context->getSwapchain()->getSwapchain();
    presentInfo.swapchainCount = 1;

    presentInfo.pWaitSemaphores = &renderFinishedSemaphores[imageIndex];
    presentInfo.waitSemaphoreCount = 1;

    presentInfo.pImageIndices = &imageIndex;

    vk::Result queueResult = context->getGraphicsQueue().presentKHR(&presentInfo);

    // Increase the number of frames drawn
    frameNumber++;

/*

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
    vk::Semaphore waitSemaphores[] = { imageAvailableSemaphores[semaphoreIndex] };
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

    currentFrame = (currentFrame + 1) % FRAME_OVERLAP;
*/
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

    // 4: Initialize ImGui for Vulkan (using dynamic rendering)
    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.Instance = static_cast<VkInstance>(context->getInstance());
    init_info.PhysicalDevice = static_cast<VkPhysicalDevice>(context->getPhysicalDevice());
    init_info.Device = static_cast<VkDevice>(context->getDevice());
    init_info.Queue = static_cast<VkQueue>(context->getGraphicsQueue());
    init_info.DescriptorPool = static_cast<VkDescriptorPool>(imguiDescriptorPool);
    init_info.MinImageCount = 3;
    init_info.ImageCount = 3;
    init_info.UseDynamicRendering = true;

    // Dynamic rendering setup
    VkPipelineRenderingCreateInfoKHR pipelineRenderingInfo = {};
    pipelineRenderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
    const auto colorAttachmentFormat = static_cast<VkFormat>(context->getSwapchain()->getImageFormat());
    pipelineRenderingInfo.colorAttachmentCount = 1;
    pipelineRenderingInfo.pColorAttachmentFormats = &colorAttachmentFormat;

    init_info.PipelineInfoMain.PipelineRenderingCreateInfo = pipelineRenderingInfo;

    ImGui_ImplVulkan_Init(&init_info);
}

void Renderer::drawImGui(vk::CommandBuffer commandBuffer) {
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    // Simple Hello World window
    ImGui::Begin("Hello ImGui");
    ImGui::Text("Hello, World!");
    ImGui::End();

    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);
}

} // namespace graphics
