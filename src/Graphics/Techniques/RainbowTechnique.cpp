#include "RainbowTechnique.h"
#include "../Renderer.h"
#include "../VulkanContext.h"
#include "../PipelineBuilder.h"
#include "../DescriptorLayoutBuilder.hpp"
#include "../DescriptorWriter.h"
#include "../Utils.hpp"
#include "BasicServices/Log.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/transform.hpp>

using services::Log;

namespace graphics::techniques {

    void RainbowTechnique::init(Renderer* renderer) {
        this->renderer = renderer;
        VulkanContext* ctx = renderer->getContext();
        vk::Device device = ctx->getDevice();

        // UBO buffer (CPU-visible, written every frame)
        uboBuffer = Buffer(ctx,
            sizeof(RainbowUBO),
            vk::BufferUsageFlagBits::eUniformBuffer,
            VMA_MEMORY_USAGE_CPU_TO_GPU);

        createDescriptors();
        createPipeline();

        Log::Info("RainbowTechnique initialized");
    }

    void RainbowTechnique::cleanup(vk::Device device) {
        uboBuffer.destroy();

        pipeline.reset();
        if (pipelineLayout) { device.destroyPipelineLayout(pipelineLayout); pipelineLayout = nullptr; }
        if (descriptorLayout) { device.destroyDescriptorSetLayout(descriptorLayout); descriptorLayout = nullptr; }
    }

    void RainbowTechnique::createDescriptors() {
        vk::Device device = renderer->getContext()->getDevice();

        DescriptorLayoutBuilder builder;
        builder.addBinding(0, vk::DescriptorType::eUniformBuffer);
        descriptorLayout = builder.build(device, vk::ShaderStageFlagBits::eFragment);

        vector<DescriptorAllocatorGrowable::PoolSizeRatio> sizes = {
            { vk::DescriptorType::eUniformBuffer, 1 }
        };
        descriptorPool = DescriptorAllocatorGrowable(device, 1, sizes);
        descriptorSet  = descriptorPool.allocate(descriptorLayout);

        // Write the UBO binding once (buffer doesn't change, only its contents)
        DescriptorWriter writer;
        writer.writeBuffer(0, uboBuffer.buffer, sizeof(RainbowUBO), 0,
                           vk::DescriptorType::eUniformBuffer);
        writer.updateSet(device, descriptorSet);
    }

    void RainbowTechnique::createPipeline() {
        vk::Device device = renderer->getContext()->getDevice();

        vk::PipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts    = &descriptorLayout;
        pipelineLayout = device.createPipelineLayout(layoutInfo);

        vk::Format colorFormat = renderer->getSceneImage().imageFormat;

        PipelineBuilder builder(renderer->getContext(),
                                "shaders/rainbow_sky.vert.spv",
                                "shaders/rainbow_sky.frag.spv");
        builder.pipelineLayout = pipelineLayout;
        builder.setColorAttachmentFormat(colorFormat);
        builder.disableDepthTest();
        builder.disableBlending();
        builder.setMultisamplingNone();
        builder.setCullMode(vk::CullModeFlagBits::eNone, vk::FrontFace::eCounterClockwise);
        pipeline = builder.buildPipeline(device);
    }

    void RainbowTechnique::render(vk::CommandBuffer cmd,
                                  const DrawContext& /*drawContext*/,
                                  const GPUSceneData& sceneData,
                                  DescriptorAllocatorGrowable& /*frameDescriptors*/) {
        // ── Update UBO ──────────────────────────────────────────────────────
        RainbowUBO ubo{};
        ubo.invViewProj = glm::inverse(sceneData.viewProj);

        // Camera position from inverse view matrix
        glm::mat4 invView = glm::inverse(sceneData.view);
        ubo.cameraPos = invView[3]; // Last column = world position

        // Sun direction: FROM observer TOWARD sun
        float elevRad = glm::radians(params.sunElevation);
        ubo.sunDir = glm::vec4(0.0f, glm::sin(elevRad), glm::cos(elevRad), 0.0f);

        ubo.dropletRadius      = params.dropletRadius;
        ubo.nBase              = params.refractiveIndex;
        ubo.primaryIntensity   = params.primaryIntensity   * params.intensityMult;
        ubo.secondaryIntensity = params.secondaryIntensity * params.intensityMult;
        ubo.showPrimary    = params.showPrimary   ? 1 : 0;
        ubo.showSecondary  = params.showSecondary ? 1 : 0;
        ubo.numWavelengths = params.numWavelengths;
        ubo.altitude       = params.altitude;

        memcpy(uboBuffer.info.pMappedData, &ubo, sizeof(RainbowUBO));

        // ── Render fullscreen sky quad ───────────────────────────────────────
        Image& sceneImage = renderer->getSceneImage();

        vk::RenderingAttachmentInfo colorAttachment{};
        colorAttachment.imageView   = sceneImage.imageView;
        colorAttachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
        colorAttachment.loadOp      = vk::AttachmentLoadOp::eClear;
        colorAttachment.storeOp     = vk::AttachmentStoreOp::eStore;
        colorAttachment.clearValue  = vk::ClearColorValue{0.0f, 0.0f, 0.0f, 1.0f};

        auto extent = renderer->getContext()->getDrawImage().imageExtent;
        vk::RenderingInfo renderInfo{};
        renderInfo.renderArea           = vk::Rect2D{{0, 0}, {extent.width, extent.height}};
        renderInfo.layerCount           = 1;
        renderInfo.colorAttachmentCount = 1;
        renderInfo.pColorAttachments    = &colorAttachment;

        cmd.beginRendering(renderInfo);
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->getPipeline());
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipelineLayout,
                               0, 1, &descriptorSet, 0, nullptr);

        vk::Viewport viewport{};
        viewport.x        = 0;
        viewport.y        = 0;
        viewport.width    = static_cast<float>(extent.width);
        viewport.height   = static_cast<float>(extent.height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        cmd.setViewport(0, viewport);

        vk::Rect2D scissor{{0, 0}, {extent.width, extent.height}};
        cmd.setScissor(0, scissor);

        cmd.draw(3, 1, 0, 0); // Fullscreen triangle — no vertex buffer needed

        cmd.endRendering();
    }

} // namespace graphics::techniques
