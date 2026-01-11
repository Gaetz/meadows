#include "GLTFMetallicRoughness.h"

#include "Graphics/DescriptorLayoutBuilder.hpp"
#include "Graphics/PipelineBuilder.h"
#include "Graphics/Renderer.h"

namespace graphics::pipelines {
    void GLTFMetallicRoughness::buildPipelines(const Renderer *renderer) {
        vk::PushConstantRange matrixRange{};
        matrixRange.offset = 0;
        matrixRange.size = sizeof(GraphicsPushConstants);
        matrixRange.stageFlags = vk::ShaderStageFlagBits::eVertex;

        DescriptorLayoutBuilder layoutBuilder;
        layoutBuilder.addBinding(0, vk::DescriptorType::eUniformBuffer);
        layoutBuilder.addBinding(1, vk::DescriptorType::eCombinedImageSampler);
        layoutBuilder.addBinding(2, vk::DescriptorType::eCombinedImageSampler);

        vk::Device device = renderer->getContext()->getDevice();
        materialLayout = layoutBuilder.build(
            device, vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment);

        const vk::DescriptorSetLayout layouts[] = {renderer->getSceneDataDescriptorLayout(), materialLayout};

        vk::PipelineLayoutCreateInfo meshLayoutInfo{};
        meshLayoutInfo.setLayoutCount = 2;
        meshLayoutInfo.pSetLayouts = layouts;
        meshLayoutInfo.pPushConstantRanges = &matrixRange;
        meshLayoutInfo.pushConstantRangeCount = 1;

        pipelineLayout = device.createPipelineLayout(meshLayoutInfo);

        PipelineBuilder pipelineBuilder{renderer->getContext(), "shaders/mesh.vert.spv", "shaders/mesh.frag.spv"};
        pipelineBuilder.setInputTopology(vk::PrimitiveTopology::eTriangleList);
        pipelineBuilder.setPolygonMode(vk::PolygonMode::eFill);
        pipelineBuilder.setCullMode(vk::CullModeFlagBits::eNone, vk::FrontFace::eClockwise);
        pipelineBuilder.setMultisamplingNone();
        pipelineBuilder.disableBlending();
        pipelineBuilder.enableDepthTest(true, vk::CompareOp::eLessOrEqual);

        // Render format
        pipelineBuilder.setColorAttachmentFormat(renderer->getContext()->getDrawImage().imageFormat);
        pipelineBuilder.setDepthFormat(renderer->getContext()->getDepthImage().imageFormat);

        // Use the pipeline layout we created
        pipelineBuilder.pipelineLayout = pipelineLayout;

        opaquePipeline = pipelineBuilder.buildPipeline(device);

        // Create the transparent variant
        pipelineBuilder.enableBlendingAdditive();
        pipelineBuilder.enableDepthTest(false, vk::CompareOp::eLessOrEqual);
        transparentPipeline = pipelineBuilder.buildPipeline(device);

        pipelineBuilder.destroyShaderModules(device);
    }

    MaterialInstance GLTFMetallicRoughness::writeMaterial(vk::Device device, MaterialPass pass,
                                                          const MaterialResources &resources,
                                                          DescriptorAllocatorGrowable* descriptorAllocator) {
        MaterialInstance matData;
        matData.passType = pass;
        if (pass == MaterialPass::Transparent) {
            matData.pipeline = transparentPipeline.get();
        } else {
            matData.pipeline = opaquePipeline.get();
        }

        matData.materialSet = descriptorAllocator->allocate(materialLayout);


        writer.clear();
        writer.writeBuffer(0, resources.dataBuffer, sizeof(MaterialConstants), resources.dataBufferOffset,
                            vk::DescriptorType::eUniformBuffer);
        writer.writeImage(1, resources.colorImage.imageView, resources.colorSampler,
                           vk::ImageLayout::eShaderReadOnlyOptimal, vk::DescriptorType::eCombinedImageSampler);
        writer.writeImage(2, resources.metalRoughImage.imageView, resources.metalRoughSampler,
                           vk::ImageLayout::eShaderReadOnlyOptimal, vk::DescriptorType::eCombinedImageSampler);
        writer.updateSet(device, matData.materialSet);

        return matData;
    }

    void GLTFMetallicRoughness::clear(vk::Device device) {
        if (pipelineLayout) {
            device.destroyPipelineLayout(pipelineLayout);
            pipelineLayout = nullptr;
        }
        if (materialLayout) {
            device.destroyDescriptorSetLayout(materialLayout);
            materialLayout = nullptr;
        }
        // Pipelines are destroyed by their unique_ptr destructors
        opaquePipeline.reset();
        transparentPipeline.reset();
    }
}
