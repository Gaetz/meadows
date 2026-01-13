/**
 * @file GLTFMetallicRoughness.cpp
 * @brief Implementation of the PBR metallic-roughness material pipeline.
 *
 * This implements the standard glTF 2.0 PBR material model, which is the
 * most widely used material format in modern 3D graphics.
 */

#include "GLTFMetallicRoughness.h"

#include "Graphics/DescriptorLayoutBuilder.hpp"
#include "Graphics/PipelineBuilder.h"
#include "Graphics/Renderer.h"

namespace graphics::pipelines {

    // =========================================================================
    // Pipeline Building
    // =========================================================================

    void GLTFMetallicRoughness::buildPipelines(const Renderer *renderer) {
        // -----------------------------------------------------------------
        // Step 1: Define push constants
        // -----------------------------------------------------------------
        // Push constants are small, fast-access data sent with each draw call
        // Here we use them for the model matrix and vertex buffer address
        vk::PushConstantRange matrixRange{};
        matrixRange.offset = 0;
        matrixRange.size = sizeof(GraphicsPushConstants);
        matrixRange.stageFlags = vk::ShaderStageFlagBits::eVertex;

        // -----------------------------------------------------------------
        // Step 2: Build the material descriptor set layout
        // -----------------------------------------------------------------
        // This defines what resources the material shader expects:
        // - Binding 0: Uniform buffer (MaterialConstants)
        // - Binding 1: Color texture + sampler
        // - Binding 2: Metallic-roughness texture + sampler
        DescriptorLayoutBuilder layoutBuilder;
        layoutBuilder.addBinding(0, vk::DescriptorType::eUniformBuffer);
        layoutBuilder.addBinding(1, vk::DescriptorType::eCombinedImageSampler);
        layoutBuilder.addBinding(2, vk::DescriptorType::eCombinedImageSampler);

        vk::Device device = renderer->getContext()->getDevice();
        materialLayout = layoutBuilder.build(
            device, vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment);

        // -----------------------------------------------------------------
        // Step 3: Create the pipeline layout
        // -----------------------------------------------------------------
        // The pipeline uses two descriptor sets:
        // - Set 0: Scene data (camera matrices, lighting) - from renderer
        // - Set 1: Material data (textures, constants) - our materialLayout
        const vk::DescriptorSetLayout layouts[] = {renderer->getSceneDataDescriptorLayout(), materialLayout};

        vk::PipelineLayoutCreateInfo meshLayoutInfo{};
        meshLayoutInfo.setLayoutCount = 2;
        meshLayoutInfo.pSetLayouts = layouts;
        meshLayoutInfo.pPushConstantRanges = &matrixRange;
        meshLayoutInfo.pushConstantRangeCount = 1;

        pipelineLayout = device.createPipelineLayout(meshLayoutInfo);

        // -----------------------------------------------------------------
        // Step 4: Build the opaque pipeline
        // -----------------------------------------------------------------
        PipelineBuilder pipelineBuilder{renderer->getContext(), "shaders/mesh.vert.spv", "shaders/mesh.frag.spv"};
        pipelineBuilder.setInputTopology(vk::PrimitiveTopology::eTriangleList);
        pipelineBuilder.setPolygonMode(vk::PolygonMode::eFill);
        pipelineBuilder.setCullMode(vk::CullModeFlagBits::eNone, vk::FrontFace::eClockwise);
        pipelineBuilder.setMultisamplingNone();
        pipelineBuilder.disableBlending();  // Opaque = no blending
        pipelineBuilder.enableDepthTest(true, vk::CompareOp::eLessOrEqual);  // Write depth

        // Set render target formats
        pipelineBuilder.setColorAttachmentFormat(renderer->getContext()->getDrawImage().imageFormat);
        pipelineBuilder.setDepthFormat(renderer->getContext()->getDepthImage().imageFormat);

        pipelineBuilder.pipelineLayout = pipelineLayout;

        opaquePipeline = pipelineBuilder.buildPipeline(device);

        // -----------------------------------------------------------------
        // Step 5: Build the transparent pipeline
        // -----------------------------------------------------------------
        // Reuse the same builder, just change blending and depth settings
        pipelineBuilder.enableBlendingAdditive();  // Additive blending for transparency
        pipelineBuilder.enableDepthTest(false, vk::CompareOp::eLessOrEqual);  // Don't write depth
        transparentPipeline = pipelineBuilder.buildPipeline(device);

        // Clean up shader modules - they're baked into the pipelines now
        pipelineBuilder.destroyShaderModules(device);
    }

    // =========================================================================
    // Material Instance Creation
    // =========================================================================

    MaterialInstance GLTFMetallicRoughness::writeMaterial(vk::Device device, MaterialPass pass,
                                                          const MaterialResources &resources,
                                                          DescriptorAllocatorGrowable* descriptorAllocator) {
        MaterialInstance matData;
        matData.passType = pass;

        // Select the appropriate pipeline based on material type
        if (pass == MaterialPass::Transparent) {
            matData.pipeline = transparentPipeline.get();
        } else {
            matData.pipeline = opaquePipeline.get();
        }

        // Allocate a descriptor set for this material instance
        matData.materialSet = descriptorAllocator->allocate(materialLayout);

        // Write all the resource bindings to the descriptor set
        writer.clear();

        // Binding 0: Material constants (uniform buffer)
        writer.writeBuffer(0, resources.dataBuffer, sizeof(MaterialConstants), resources.dataBufferOffset,
                            vk::DescriptorType::eUniformBuffer);

        // Binding 1: Base color texture
        writer.writeImage(1, resources.colorImage.imageView, resources.colorSampler,
                           vk::ImageLayout::eShaderReadOnlyOptimal, vk::DescriptorType::eCombinedImageSampler);

        // Binding 2: Metallic-roughness texture
        writer.writeImage(2, resources.metalRoughImage.imageView, resources.metalRoughSampler,
                           vk::ImageLayout::eShaderReadOnlyOptimal, vk::DescriptorType::eCombinedImageSampler);

        // Apply all writes to the descriptor set
        writer.updateSet(device, matData.materialSet);

        return matData;
    }

    // =========================================================================
    // Cleanup
    // =========================================================================

    void GLTFMetallicRoughness::clear(vk::Device device) {
        if (pipelineLayout) {
            device.destroyPipelineLayout(pipelineLayout);
            pipelineLayout = nullptr;
        }
        if (materialLayout) {
            device.destroyDescriptorSetLayout(materialLayout);
            materialLayout = nullptr;
        }
        // Pipelines are automatically destroyed by their unique_ptr destructors
        opaquePipeline.reset();
        transparentPipeline.reset();
    }
}
