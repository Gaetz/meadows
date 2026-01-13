#include "DeferredRenderingTechnique.h"
#include "../Renderer.h"
#include "../PipelineBuilder.h"
#include "../DescriptorLayoutBuilder.hpp"
#include "../DescriptorWriter.h"
#include "../Utils.hpp"
#include "../VulkanContext.h"
#include "BasicServices/Log.h"
#include "../VulkanInit.hpp"

using services::Log;

namespace graphics::techniques {

    void GBuffer::init(VulkanContext* context, vk::Extent3D extent) {
        this->extent = extent;
        position = Image(context, extent, vk::Format::eR16G16B16A16Sfloat, vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled);
        normal = Image(context, extent, vk::Format::eR16G16B16A16Sfloat, vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled);
        albedo = Image(context, extent, vk::Format::eR8G8B8A8Unorm, vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled);
    }

    void GBuffer::destroy(VulkanContext* context) {
        position.destroy(context);
        normal.destroy(context);
        albedo.destroy(context);
    }

    void DeferredRenderingTechnique::init(Renderer* renderer) {
        this->renderer = renderer;
        startTime = std::chrono::high_resolution_clock::now();

        // Create lights buffer
        lightsBuffer = Buffer(renderer->getContext(), sizeof(DeferredLightsData),
            vk::BufferUsageFlagBits::eUniformBuffer, VMA_MEMORY_USAGE_CPU_TO_GPU);

        // Initialize lights (like Sascha Willems example, scaled up for the 30x model)
        lightsData.numLights = 6;
        const float scale = 30.0f;  // Match the model scale

        // White light
        lightsData.lights[0].position = Vec4(0.0f, 0.0f, 1.0f, 0.0f) * scale;
        lightsData.lights[0].colorRadius = Vec4(1.5f, 1.5f, 1.5f, 15.0f * scale);

        // Red light
        lightsData.lights[1].position = Vec4(-2.0f, 0.0f, 0.0f, 0.0f) * scale;
        lightsData.lights[1].colorRadius = Vec4(1.0f, 0.0f, 0.0f, 15.0f * scale);

        // Blue light
        lightsData.lights[2].position = Vec4(2.0f, -1.0f, 0.0f, 0.0f) * scale;
        lightsData.lights[2].colorRadius = Vec4(0.0f, 0.0f, 2.5f, 10.0f * scale);

        // Yellow light
        lightsData.lights[3].position = Vec4(0.0f, -0.9f, 0.5f, 0.0f) * scale;
        lightsData.lights[3].colorRadius = Vec4(1.0f, 1.0f, 0.0f, 5.0f * scale);

        // Green light
        lightsData.lights[4].position = Vec4(0.0f, -0.5f, 0.0f, 0.0f) * scale;
        lightsData.lights[4].colorRadius = Vec4(0.0f, 1.0f, 0.2f, 10.0f * scale);

        // Orange light
        lightsData.lights[5].position = Vec4(0.0f, -1.0f, 0.0f, 0.0f) * scale;
        lightsData.lights[5].colorRadius = Vec4(1.0f, 0.7f, 0.3f, 30.0f * scale);

        createGBuffer();
        createDescriptors();
        createPipelines();
    }

    void DeferredRenderingTechnique::cleanup(vk::Device device) {
        gBuffer.destroy(renderer->getContext());
        lightsBuffer.destroy();
        device.destroyDescriptorSetLayout(gBufferDescriptorLayout);
        device.destroyDescriptorSetLayout(deferredDescriptorLayout);
        device.destroyPipelineLayout(gBufferLayout);
        device.destroyPipelineLayout(deferredLayout);
    }

    void DeferredRenderingTechnique::createGBuffer() {
        auto extent = renderer->getContext()->getDrawImage().imageExtent;
        gBuffer.init(renderer->getContext(), extent);
    }

    void DeferredRenderingTechnique::createDescriptors() {
        vk::Device device = renderer->getContext()->getDevice();

        // G-Buffer pass descriptors (Scene Data)
        DescriptorLayoutBuilder gBufferBuilder;
        gBufferBuilder.addBinding(0, vk::DescriptorType::eUniformBuffer); // SceneData
        gBufferDescriptorLayout = gBufferBuilder.build(device, vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment);

        // Deferred pass descriptors (G-Buffer textures + Lights)
        DescriptorLayoutBuilder deferredBuilder;
        deferredBuilder.addBinding(0, vk::DescriptorType::eCombinedImageSampler); // Position
        deferredBuilder.addBinding(1, vk::DescriptorType::eCombinedImageSampler); // Normal
        deferredBuilder.addBinding(2, vk::DescriptorType::eCombinedImageSampler); // Albedo
        deferredBuilder.addBinding(3, vk::DescriptorType::eUniformBuffer);        // Lights
        deferredDescriptorLayout = deferredBuilder.build(device, vk::ShaderStageFlagBits::eFragment);

        // Allocate and update deferred descriptor set
        gBufferDescriptorSet = renderer->getContext()->getGlobalDescriptorAllocator()->allocate(deferredDescriptorLayout);

        DescriptorWriter writer;
        writer.writeImage(0, gBuffer.position.imageView, renderer->defaultSamplerLinear, vk::ImageLayout::eShaderReadOnlyOptimal, vk::DescriptorType::eCombinedImageSampler);
        writer.writeImage(1, gBuffer.normal.imageView, renderer->defaultSamplerLinear, vk::ImageLayout::eShaderReadOnlyOptimal, vk::DescriptorType::eCombinedImageSampler);
        writer.writeImage(2, gBuffer.albedo.imageView, renderer->defaultSamplerLinear, vk::ImageLayout::eShaderReadOnlyOptimal, vk::DescriptorType::eCombinedImageSampler);
        writer.writeBuffer(3, lightsBuffer.buffer, sizeof(DeferredLightsData), 0, vk::DescriptorType::eUniformBuffer);
        writer.updateSet(device, gBufferDescriptorSet);
    }

    void DeferredRenderingTechnique::createPipelines() {
        vk::Device device = renderer->getContext()->getDevice();

        // G-Buffer Pipeline
        vk::PushConstantRange pushConstant;
        pushConstant.offset = 0;
        pushConstant.size = sizeof(GraphicsPushConstants);
        pushConstant.stageFlags = vk::ShaderStageFlagBits::eVertex;

        vk::DescriptorSetLayout layouts[] = { renderer->getSceneDataDescriptorLayout(), renderer->metalRoughMaterial.materialLayout };
        vk::PipelineLayoutCreateInfo gBufferLayoutInfo;
        gBufferLayoutInfo.setLayoutCount = 2;
        gBufferLayoutInfo.pSetLayouts = layouts;
        gBufferLayoutInfo.pushConstantRangeCount = 1;
        gBufferLayoutInfo.pPushConstantRanges = &pushConstant;
        gBufferLayout = device.createPipelineLayout(gBufferLayoutInfo);

        PipelineBuilder gBufferBuilder(renderer->getContext(), "shaders/g_buffer.vert.spv", "shaders/g_buffer.frag.spv");
        gBufferBuilder.pipelineLayout = gBufferLayout;
        vk::Format formats[] = { gBuffer.position.imageFormat, gBuffer.normal.imageFormat, gBuffer.albedo.imageFormat };
        gBufferBuilder.setColorAttachmentFormats(formats);
        gBufferBuilder.setDepthFormat(renderer->getContext()->getDepthImage().imageFormat);
        gBufferBuilder.enableDepthTest(true, vk::CompareOp::eLessOrEqual);
        gBufferBuilder.setCullMode(vk::CullModeFlagBits::eBack, vk::FrontFace::eCounterClockwise);
        gBufferBuilder.disableBlending();
        gBufferBuilder.setMultisamplingNone();
        gBufferPipeline = gBufferBuilder.buildPipeline(device);

        // Deferred Pipeline
        vk::PushConstantRange deferredPushConstant;
        deferredPushConstant.offset = 0;
        deferredPushConstant.size = sizeof(uint32_t);
        deferredPushConstant.stageFlags = vk::ShaderStageFlagBits::eFragment;

        vk::DescriptorSetLayout deferredLayouts[] = { renderer->getSceneDataDescriptorLayout(), renderer->metalRoughMaterial.materialLayout, deferredDescriptorLayout };
        vk::PipelineLayoutCreateInfo deferredLayoutInfo;
        deferredLayoutInfo.setLayoutCount = 3;
        deferredLayoutInfo.pSetLayouts = deferredLayouts;
        deferredLayoutInfo.pushConstantRangeCount = 1;
        deferredLayoutInfo.pPushConstantRanges = &deferredPushConstant;
        deferredLayout = device.createPipelineLayout(deferredLayoutInfo);

        PipelineBuilder deferredBuilder(renderer->getContext(), "shaders/deferred.vert.spv", "shaders/deferred.frag.spv");
        deferredBuilder.pipelineLayout = deferredLayout;
        deferredBuilder.setColorAttachmentFormat(renderer->getContext()->getDrawImage().imageFormat);
        deferredBuilder.disableDepthTest();
        deferredBuilder.disableBlending();
        deferredBuilder.setMultisamplingNone();
        deferredBuilder.setCullMode(vk::CullModeFlagBits::eNone, vk::FrontFace::eCounterClockwise);
        deferredPipeline = deferredBuilder.buildPipeline(device);
    }

    void DeferredRenderingTechnique::render(
        vk::CommandBuffer cmd,
        const DrawContext& drawContext,
        const GPUSceneData& sceneData,
        DescriptorAllocatorGrowable& frameDescriptors
    ) {
        static bool loggedOnce = false;
        if (!loggedOnce) {
            Log::Debug("Deferred: opaqueSurfaces=%zu, transparentSurfaces=%zu",
                drawContext.opaqueSurfaces.size(), drawContext.transparentSurfaces.size());
            loggedOnce = true;
        }

        // 1. Geometry Pass
        // Transition G-Buffer images to color attachment
        graphics::transitionImage(cmd, gBuffer.position.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal);
        graphics::transitionImage(cmd, gBuffer.normal.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal);
        graphics::transitionImage(cmd, gBuffer.albedo.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal);

        vk::ClearValue clearValues[3];
        clearValues[0].color = vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f});
        clearValues[1].color = vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f});
        clearValues[2].color = vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f});

        vk::RenderingAttachmentInfo colorAttachments[3];
        colorAttachments[0] = graphics::attachmentInfo(gBuffer.position.imageView, &clearValues[0], vk::ImageLayout::eColorAttachmentOptimal);
        colorAttachments[1] = graphics::attachmentInfo(gBuffer.normal.imageView, &clearValues[1], vk::ImageLayout::eColorAttachmentOptimal);
        colorAttachments[2] = graphics::attachmentInfo(gBuffer.albedo.imageView, &clearValues[2], vk::ImageLayout::eColorAttachmentOptimal);

        vk::RenderingAttachmentInfo depthAttachment = graphics::depthAttachmentInfo(renderer->getContext()->getDepthImage().imageView, vk::ImageLayout::eDepthAttachmentOptimal);

        vk::RenderingInfo renderInfo;
        renderInfo.renderArea = vk::Rect2D({0, 0}, {gBuffer.extent.width, gBuffer.extent.height});
        renderInfo.layerCount = 1;
        renderInfo.colorAttachmentCount = 3;
        renderInfo.pColorAttachments = colorAttachments;
        renderInfo.pDepthAttachment = &depthAttachment;

        cmd.beginRendering(&renderInfo);

        vk::Viewport viewport(0, 0, (float)gBuffer.extent.width, (float)gBuffer.extent.height, 0, 1);
        cmd.setViewport(0, 1, &viewport);
        vk::Rect2D scissor({0, 0}, {gBuffer.extent.width, gBuffer.extent.height});
        cmd.setScissor(0, 1, &scissor);

        // Bind G-Buffer pipeline
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, gBufferPipeline->getPipeline());

        // Bind Scene Data
        vk::DescriptorSet sceneDescriptor = frameDescriptors.allocate(renderer->getSceneDataDescriptorLayout());
        {
            DescriptorWriter writer;
            writer.writeBuffer(0, renderer->getSceneDataBuffer().buffer, sizeof(GPUSceneData), 0, vk::DescriptorType::eUniformBuffer);
            writer.updateSet(renderer->getContext()->getDevice(), sceneDescriptor);
        }
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, gBufferLayout, 0, 1, &sceneDescriptor, 0, nullptr);

        // Draw opaque surfaces
        for (const auto& r : drawContext.opaqueSurfaces) {
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, gBufferLayout, 1, 1, &r.material->materialSet, 0, nullptr);
            cmd.bindIndexBuffer(r.indexBuffer, 0, vk::IndexType::eUint32);

            GraphicsPushConstants pushConstants;
            pushConstants.vertexBuffer = r.vertexBufferAddress;
            pushConstants.worldMatrix = r.transform;
            cmd.pushConstants(gBufferLayout, vk::ShaderStageFlagBits::eVertex, 0, sizeof(GraphicsPushConstants), &pushConstants);

            cmd.drawIndexed(r.indexCount, 1, r.firstIndex, 0, 0);
        }

        cmd.endRendering();

        // 2. Transition G-Buffer to shader read
        graphics::transitionImage(cmd, gBuffer.position.image, vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::eShaderReadOnlyOptimal);
        graphics::transitionImage(cmd, gBuffer.normal.image, vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::eShaderReadOnlyOptimal);
        graphics::transitionImage(cmd, gBuffer.albedo.image, vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::eShaderReadOnlyOptimal);

        // Update animated lights
        updateLights(sceneData);

        // 3. Deferred Pass - Render to Renderer's scene image (for post-processing)
        Image& sceneImage = renderer->getSceneImage();

        vk::RenderingAttachmentInfo sceneColorAttachment = graphics::attachmentInfo(sceneImage.imageView, nullptr, vk::ImageLayout::eColorAttachmentOptimal);
        vk::RenderingInfo deferredRenderInfo;
        deferredRenderInfo.renderArea = vk::Rect2D({0, 0}, {gBuffer.extent.width, gBuffer.extent.height});
        deferredRenderInfo.layerCount = 1;
        deferredRenderInfo.colorAttachmentCount = 1;
        deferredRenderInfo.pColorAttachments = &sceneColorAttachment;

        cmd.beginRendering(&deferredRenderInfo);

        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, deferredPipeline->getPipeline());

        // Bind Scene Data at set 0
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, deferredLayout, 0, 1, &sceneDescriptor, 0, nullptr);
        // Bind G-Buffer descriptors at set 2
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, deferredLayout, 2, 1, &gBufferDescriptorSet, 0, nullptr);

        // Pass debug mode via push constants
        uint32_t mode = static_cast<uint32_t>(debugMode);
        cmd.pushConstants(deferredLayout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(uint32_t), &mode);

        // Draw full-screen quad
        cmd.draw(3, 1, 0, 0);

        cmd.endRendering();
    }

    void DeferredRenderingTechnique::updateLights(const GPUSceneData& sceneData) {
        // Get elapsed time
        auto now = std::chrono::high_resolution_clock::now();
        float timer = std::chrono::duration<float>(now - startTime).count();

        // Extract camera position from inverse view matrix
        Mat4 invView = glm::inverse(sceneData.view);
        lightsData.viewPos = Vec4(invView[3]);

        const float scale = 30.0f;  // Match the model scale

        // Animate lights (like Sascha Willems example, scaled up)
        // White light - orbits around center
        lightsData.lights[0].position.x = sin(glm::radians(360.0f * timer)) * 5.0f * scale;
        lightsData.lights[0].position.z = cos(glm::radians(360.0f * timer)) * 5.0f * scale;

        // Red light
        lightsData.lights[1].position.x = (-4.0f + sin(glm::radians(360.0f * timer) + 45.0f) * 2.0f) * scale;
        lightsData.lights[1].position.z = (0.0f + cos(glm::radians(360.0f * timer) + 45.0f) * 2.0f) * scale;

        // Blue light
        lightsData.lights[2].position.x = (4.0f + sin(glm::radians(360.0f * timer)) * 2.0f) * scale;
        lightsData.lights[2].position.z = (0.0f + cos(glm::radians(360.0f * timer)) * 2.0f) * scale;

        // Green light
        lightsData.lights[4].position.x = (0.0f + sin(glm::radians(360.0f * timer + 90.0f)) * 5.0f) * scale;
        lightsData.lights[4].position.z = (0.0f - cos(glm::radians(360.0f * timer + 45.0f)) * 5.0f) * scale;

        // Copy to GPU buffer
        memcpy(lightsBuffer.info.pMappedData, &lightsData, sizeof(DeferredLightsData));
    }

} // namespace graphics::techniques
