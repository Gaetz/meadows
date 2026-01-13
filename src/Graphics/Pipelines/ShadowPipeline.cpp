/**
 * @file ShadowPipeline.cpp
 * @brief Implementation of shadow mapping pipelines.
 *
 * Shadow mapping is a fundamental technique for real-time shadows.
 * This implementation uses a simple shadow map with depth bias to prevent artifacts.
 */

#include "ShadowPipeline.h"

#include "Graphics/DescriptorLayoutBuilder.hpp"
#include "Graphics/PipelineBuilder.h"
#include "Graphics/Renderer.h"
#include "Graphics/ShadowMap.h"
#include "Graphics/Utils.hpp"
#include "BasicServices/File.h"

namespace graphics::pipelines {

    // =========================================================================
    // Main Build Function
    // =========================================================================

    void ShadowPipeline::buildPipelines(const Renderer* renderer) {
        vk::Device device = renderer->getContext()->getDevice();

        // Build all three pipelines
        buildDepthPipeline(renderer, device);
        buildShadowMeshPipeline(renderer, device);
        buildDebugPipeline(renderer, device);
    }

    // =========================================================================
    // Depth Pipeline (Shadow Map Generation)
    // =========================================================================

    void ShadowPipeline::buildDepthPipeline(const Renderer* renderer, vk::Device device) {
        // -----------------------------------------------------------------
        // Push constants for model matrix and vertex buffer address
        // -----------------------------------------------------------------
        vk::PushConstantRange matrixRange{};
        matrixRange.offset = 0;
        matrixRange.size = sizeof(GraphicsPushConstants);
        matrixRange.stageFlags = vk::ShaderStageFlagBits::eVertex;

        // -----------------------------------------------------------------
        // Pipeline layout - only needs scene data (set 0) for light matrices
        // -----------------------------------------------------------------
        // No material data needed - we're just writing depth values
        const vk::DescriptorSetLayout layouts[] = { renderer->getSceneDataDescriptorLayout() };

        vk::PipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = layouts;
        layoutInfo.pPushConstantRanges = &matrixRange;
        layoutInfo.pushConstantRangeCount = 1;

        depthPipelineLayout = device.createPipelineLayout(layoutInfo);

        // -----------------------------------------------------------------
        // Build the depth-only pipeline
        // -----------------------------------------------------------------
        PipelineBuilder builder(renderer->getContext());

        // Load only vertex shader - no fragment shader needed for depth-only
        // The depth is written automatically by the rasterizer
        vk::ShaderModule vertShader = graphics::createShaderModule(
            services::File::readBinary("shaders/shadowDepth.vert.spv"),
            device
        );
        builder.setVertexShaderOnly(vertShader);

        builder.setInputTopology(vk::PrimitiveTopology::eTriangleList);
        builder.setPolygonMode(vk::PolygonMode::eFill);

        // No culling - we want to capture shadows from all geometry
        // Front-face culling could miss thin objects seen from behind
        builder.setCullMode(vk::CullModeFlagBits::eNone, vk::FrontFace::eClockwise);
        builder.setMultisamplingNone();
        builder.disableBlending();
        builder.enableDepthTest(true, vk::CompareOp::eLessOrEqual);

        // Enable depth bias to prevent shadow acne
        // Shadow acne occurs when a surface incorrectly shadows itself due to
        // floating-point precision issues. Depth bias pushes the depth slightly
        // away from the light, eliminating the self-shadowing artifacts.
        builder.setDepthBiasEnable(true);

        // Depth-only mode - no color attachments at all
        builder.setDepthOnlyMode(true);
        builder.setDepthFormat(vk::Format::eD16Unorm);  // 16-bit depth is sufficient for shadow maps

        builder.pipelineLayout = depthPipelineLayout;

        depthPipeline = builder.buildPipeline(device);

        // Clean up shader module
        device.destroyShaderModule(vertShader);
    }

    // =========================================================================
    // Shadow Mesh Pipeline (Scene Rendering with Shadows)
    // =========================================================================

    void ShadowPipeline::buildShadowMeshPipeline(const Renderer* renderer, vk::Device device) {
        vk::PushConstantRange matrixRange{};
        matrixRange.offset = 0;
        matrixRange.size = sizeof(GraphicsPushConstants);
        matrixRange.stageFlags = vk::ShaderStageFlagBits::eVertex;

        // -----------------------------------------------------------------
        // Material descriptor layout (same as GLTFMetallicRoughness)
        // -----------------------------------------------------------------
        // Binding 0: Material constants (uniform buffer)
        // Binding 1: Base color texture
        // Binding 2: Metallic-roughness texture
        DescriptorLayoutBuilder layoutBuilder;
        layoutBuilder.addBinding(0, vk::DescriptorType::eUniformBuffer);
        layoutBuilder.addBinding(1, vk::DescriptorType::eCombinedImageSampler);
        layoutBuilder.addBinding(2, vk::DescriptorType::eCombinedImageSampler);

        materialLayout = layoutBuilder.build(
            device, vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment);

        // -----------------------------------------------------------------
        // Pipeline layout with shadow map access
        // -----------------------------------------------------------------
        // Set 0: Scene data WITH shadow map (uses special shadow descriptor layout)
        // Set 1: Material data (textures, constants)
        const vk::DescriptorSetLayout layouts[] = {
            renderer->getShadowSceneDataDescriptorLayout(),  // Includes shadow map sampler
            materialLayout
        };

        vk::PipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.setLayoutCount = 2;
        layoutInfo.pSetLayouts = layouts;
        layoutInfo.pPushConstantRanges = &matrixRange;
        layoutInfo.pushConstantRangeCount = 1;

        shadowMeshPipelineLayout = device.createPipelineLayout(layoutInfo);

        // -----------------------------------------------------------------
        // Build the shadow mesh pipeline
        // -----------------------------------------------------------------
        // Uses special shaders that sample the shadow map
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

    // =========================================================================
    // Debug Pipeline (Shadow Map Visualization)
    // =========================================================================

    void ShadowPipeline::buildDebugPipeline(const Renderer* renderer, vk::Device device) {
        // Uses scene data with shadow map for visualization
        const vk::DescriptorSetLayout layouts[] = { renderer->getShadowSceneDataDescriptorLayout() };

        vk::PipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = layouts;
        layoutInfo.pPushConstantRanges = nullptr;  // No push constants needed
        layoutInfo.pushConstantRangeCount = 0;

        debugPipelineLayout = device.createPipelineLayout(layoutInfo);

        // Create a simple fullscreen quad pipeline
        PipelineBuilder builder(renderer->getContext(),
            "shaders/shadowDebug.vert.spv",
            "shaders/shadowDebug.frag.spv");

        builder.setInputTopology(vk::PrimitiveTopology::eTriangleList);
        builder.setPolygonMode(vk::PolygonMode::eFill);
        builder.setCullMode(vk::CullModeFlagBits::eNone, vk::FrontFace::eClockwise);
        builder.setMultisamplingNone();
        builder.disableBlending();
        builder.disableDepthTest();  // Fullscreen quad doesn't need depth testing

        builder.setColorAttachmentFormat(renderer->getContext()->getDrawImage().imageFormat);
        builder.setDepthFormat(renderer->getContext()->getDepthImage().imageFormat);

        builder.pipelineLayout = debugPipelineLayout;

        debugPipeline = builder.buildPipeline(device);

        builder.destroyShaderModules(device);
    }

    // =========================================================================
    // Cleanup
    // =========================================================================

    void ShadowPipeline::clear(vk::Device device) {
        // Destroy pipeline layouts
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

        // Destroy descriptor set layout
        if (materialLayout) {
            device.destroyDescriptorSetLayout(materialLayout);
            materialLayout = nullptr;
        }

        // Pipelines are automatically destroyed by unique_ptr
        depthPipeline.reset();
        shadowMeshPipeline.reset();
        debugPipeline.reset();
    }

} // namespace graphics::pipelines
