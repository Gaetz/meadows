#include "ShadowPipeline.h"

#include "Graphics/DescriptorLayoutBuilder.hpp"
#include "Graphics/PipelineBuilder.h"
#include "Graphics/Renderer.h"
#include "Graphics/ShadowMap.h"
#include "Graphics/Utils.hpp"
#include "BasicServices/File.h"

namespace graphics::pipelines {

    void ShadowPipeline::buildPipelines(const Renderer* renderer) {
        vk::Device device = renderer->getContext()->getDevice();

        buildDepthPipeline(renderer, device);
        buildShadowMeshPipeline(renderer, device);
        buildDebugPipeline(renderer, device);
    }

    void ShadowPipeline::buildDepthPipeline(const Renderer* renderer, vk::Device device) {
        // Push constants for world matrix and vertex buffer
        vk::PushConstantRange matrixRange{};
        matrixRange.offset = 0;
        matrixRange.size = sizeof(GraphicsPushConstants);
        matrixRange.stageFlags = vk::ShaderStageFlagBits::eVertex;

        // Depth pipeline only needs scene data (set 0) for light space matrix
        const vk::DescriptorSetLayout layouts[] = { renderer->getSceneDataDescriptorLayout() };

        vk::PipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = layouts;
        layoutInfo.pPushConstantRanges = &matrixRange;
        layoutInfo.pushConstantRangeCount = 1;

        depthPipelineLayout = device.createPipelineLayout(layoutInfo);

        // Create depth-only pipeline
        PipelineBuilder builder(renderer->getContext());

        // Load only vertex shader for depth pass
        vk::ShaderModule vertShader = graphics::createShaderModule(
            services::File::readBinary("shaders/shadowDepth.vert.spv"),
            device
        );
        builder.setVertexShaderOnly(vertShader);

        builder.setInputTopology(vk::PrimitiveTopology::eTriangleList);
        builder.setPolygonMode(vk::PolygonMode::eFill);
        // No culling for shadow map - capture all geometry
        builder.setCullMode(vk::CullModeFlagBits::eNone, vk::FrontFace::eClockwise);
        builder.setMultisamplingNone();
        builder.disableBlending();
        builder.enableDepthTest(true, vk::CompareOp::eLessOrEqual);

        // Enable depth bias to prevent shadow acne
        builder.setDepthBiasEnable(true);

        // Depth-only mode - no color attachments
        builder.setDepthOnlyMode(true);
        builder.setDepthFormat(vk::Format::eD16Unorm);

        builder.pipelineLayout = depthPipelineLayout;

        depthPipeline = builder.buildPipeline(device);

        device.destroyShaderModule(vertShader);
    }

    void ShadowPipeline::buildShadowMeshPipeline(const Renderer* renderer, vk::Device device) {
        vk::PushConstantRange matrixRange{};
        matrixRange.offset = 0;
        matrixRange.size = sizeof(GraphicsPushConstants);
        matrixRange.stageFlags = vk::ShaderStageFlagBits::eVertex;

        // Build material layout (same structure as GLTFMetallicRoughness)
        DescriptorLayoutBuilder layoutBuilder;
        layoutBuilder.addBinding(0, vk::DescriptorType::eUniformBuffer);
        layoutBuilder.addBinding(1, vk::DescriptorType::eCombinedImageSampler);
        layoutBuilder.addBinding(2, vk::DescriptorType::eCombinedImageSampler);

        materialLayout = layoutBuilder.build(
            device, vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment);

        // Uses scene data with shadow map (set 0) and material (set 1)
        const vk::DescriptorSetLayout layouts[] = {
            renderer->getShadowSceneDataDescriptorLayout(),
            materialLayout
        };

        vk::PipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.setLayoutCount = 2;
        layoutInfo.pSetLayouts = layouts;
        layoutInfo.pPushConstantRanges = &matrixRange;
        layoutInfo.pushConstantRangeCount = 1;

        shadowMeshPipelineLayout = device.createPipelineLayout(layoutInfo);

        // Create shadow mesh pipeline
        PipelineBuilder builder(renderer->getContext(),
            "shaders/meshShadow.vert.spv",
            "shaders/meshShadow.frag.spv");

        builder.setInputTopology(vk::PrimitiveTopology::eTriangleList);
        builder.setPolygonMode(vk::PolygonMode::eFill);
        builder.setCullMode(vk::CullModeFlagBits::eNone, vk::FrontFace::eClockwise);
        builder.setMultisamplingNone();
        builder.disableBlending();
        builder.enableDepthTest(true, vk::CompareOp::eLessOrEqual);

        builder.setColorAttachmentFormat(renderer->getContext()->getDrawImage().imageFormat);
        builder.setDepthFormat(renderer->getContext()->getDepthImage().imageFormat);

        builder.pipelineLayout = shadowMeshPipelineLayout;

        shadowMeshPipeline = builder.buildPipeline(device);

        builder.destroyShaderModules(device);
    }

    void ShadowPipeline::buildDebugPipeline(const Renderer* renderer, vk::Device device) {
        // Debug pipeline uses scene data with shadow map (set 0)
        const vk::DescriptorSetLayout layouts[] = { renderer->getShadowSceneDataDescriptorLayout() };

        vk::PipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = layouts;
        layoutInfo.pPushConstantRanges = nullptr;
        layoutInfo.pushConstantRangeCount = 0;

        debugPipelineLayout = device.createPipelineLayout(layoutInfo);

        // Create debug visualization pipeline
        PipelineBuilder builder(renderer->getContext(),
            "shaders/shadowDebug.vert.spv",
            "shaders/shadowDebug.frag.spv");

        builder.setInputTopology(vk::PrimitiveTopology::eTriangleList);
        builder.setPolygonMode(vk::PolygonMode::eFill);
        builder.setCullMode(vk::CullModeFlagBits::eNone, vk::FrontFace::eClockwise);
        builder.setMultisamplingNone();
        builder.disableBlending();
        builder.disableDepthTest();

        builder.setColorAttachmentFormat(renderer->getContext()->getDrawImage().imageFormat);
        builder.setDepthFormat(renderer->getContext()->getDepthImage().imageFormat);

        builder.pipelineLayout = debugPipelineLayout;

        debugPipeline = builder.buildPipeline(device);

        builder.destroyShaderModules(device);
    }

    void ShadowPipeline::clear(vk::Device device) {
        if (depthPipelineLayout) {
            device.destroyPipelineLayout(depthPipelineLayout);
            depthPipelineLayout = nullptr;
        }
        if (shadowMeshPipelineLayout) {
            device.destroyPipelineLayout(shadowMeshPipelineLayout);
            shadowMeshPipelineLayout = nullptr;
        }
        if (debugPipelineLayout) {
            device.destroyPipelineLayout(debugPipelineLayout);
            debugPipelineLayout = nullptr;
        }
        if (materialLayout) {
            device.destroyDescriptorSetLayout(materialLayout);
            materialLayout = nullptr;
        }

        depthPipeline.reset();
        shadowMeshPipeline.reset();
        debugPipeline.reset();
    }

} // namespace graphics::pipelines
