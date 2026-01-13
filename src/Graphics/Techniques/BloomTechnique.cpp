#include "BloomTechnique.h"
#include "../Renderer.h"
#include "../VulkanContext.h"
#include "../PipelineBuilder.h"
#include "../DescriptorLayoutBuilder.hpp"
#include "../DescriptorWriter.h"
#include "../Utils.hpp"
#include "../VulkanInit.hpp"
#include "BasicServices/Log.h"

using services::Log;

namespace graphics::techniques {

    void BloomTechnique::init(Renderer* renderer, uint32_t width, uint32_t height) {
        this->renderer = renderer;
        this->context = renderer->getContext();
        this->width = width;
        this->height = height;

        // Create sampler for bloom textures
        vk::SamplerCreateInfo samplerInfo{};
        samplerInfo.magFilter = vk::Filter::eLinear;
        samplerInfo.minFilter = vk::Filter::eLinear;
        samplerInfo.mipmapMode = vk::SamplerMipmapMode::eLinear;
        samplerInfo.addressModeU = vk::SamplerAddressMode::eClampToEdge;
        samplerInfo.addressModeV = vk::SamplerAddressMode::eClampToEdge;
        samplerInfo.addressModeW = vk::SamplerAddressMode::eClampToEdge;
        samplerInfo.maxLod = 1.0f;
        bloomSampler = context->getDevice().createSampler(samplerInfo);

        // Create blur params buffer
        blurParamsBuffer = Buffer(context, sizeof(float) * 2,
            vk::BufferUsageFlagBits::eUniformBuffer, VMA_MEMORY_USAGE_CPU_TO_GPU);

        createImages();
        createDescriptors();
        createPipelines();

        Log::Info("Bloom technique initialized (%dx%d)", width, height);
    }

    void BloomTechnique::cleanup(vk::Device device) {
        destroyImages();
        blurParamsBuffer.destroy();

        if (bloomSampler) {
            device.destroySampler(bloomSampler);
            bloomSampler = nullptr;
        }

        device.destroyDescriptorSetLayout(singleImageLayout);
        device.destroyDescriptorSetLayout(blurDescriptorLayout);
        device.destroyDescriptorSetLayout(compositeDescriptorLayout);

        device.destroyPipelineLayout(brightPassLayout);
        device.destroyPipelineLayout(blurLayout);
        device.destroyPipelineLayout(compositeLayout);
    }

    void BloomTechnique::resize(uint32_t newWidth, uint32_t newHeight) {
        if (width == newWidth && height == newHeight) return;

        width = newWidth;
        height = newHeight;

        destroyImages();
        createImages();
    }

    void BloomTechnique::createImages() {
        // Use half resolution for bloom (better performance)
        uint32_t bloomWidth = width / 2;
        uint32_t bloomHeight = height / 2;

        vk::Extent3D extent = { bloomWidth, bloomHeight, 1 };
        vk::Format format = vk::Format::eR16G16B16A16Sfloat;
        vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eColorAttachment |
                                    vk::ImageUsageFlagBits::eSampled;

        brightPassImage = Image(context, extent, format, usage);
        blurImageH = Image(context, extent, format, usage);
        blurImageV = Image(context, extent, format, usage);
    }

    void BloomTechnique::destroyImages() {
        brightPassImage.destroy(context);
        blurImageH.destroy(context);
        blurImageV.destroy(context);
    }

    void BloomTechnique::createDescriptors() {
        vk::Device device = context->getDevice();

        // Single image layout (for bright pass)
        DescriptorLayoutBuilder singleBuilder;
        singleBuilder.addBinding(0, vk::DescriptorType::eCombinedImageSampler);
        singleImageLayout = singleBuilder.build(device, vk::ShaderStageFlagBits::eFragment);

        // Blur layout (image + params)
        DescriptorLayoutBuilder blurBuilder;
        blurBuilder.addBinding(0, vk::DescriptorType::eCombinedImageSampler);
        blurBuilder.addBinding(1, vk::DescriptorType::eUniformBuffer);
        blurDescriptorLayout = blurBuilder.build(device, vk::ShaderStageFlagBits::eFragment);

        // Composite layout (scene + bloom)
        DescriptorLayoutBuilder compositeBuilder;
        compositeBuilder.addBinding(0, vk::DescriptorType::eCombinedImageSampler);
        compositeBuilder.addBinding(1, vk::DescriptorType::eCombinedImageSampler);
        compositeDescriptorLayout = compositeBuilder.build(device, vk::ShaderStageFlagBits::eFragment);
    }

    void BloomTechnique::createPipelines() {
        vk::Device device = context->getDevice();

        // Bright pass pipeline layout
        vk::PushConstantRange brightPassPush{};
        brightPassPush.stageFlags = vk::ShaderStageFlagBits::eFragment;
        brightPassPush.offset = 0;
        brightPassPush.size = sizeof(float) * 2; // threshold, intensity

        vk::PipelineLayoutCreateInfo brightPassLayoutInfo{};
        brightPassLayoutInfo.setLayoutCount = 1;
        brightPassLayoutInfo.pSetLayouts = &singleImageLayout;
        brightPassLayoutInfo.pushConstantRangeCount = 1;
        brightPassLayoutInfo.pPushConstantRanges = &brightPassPush;
        brightPassLayout = device.createPipelineLayout(brightPassLayoutInfo);

        // Blur pipeline layout
        vk::PipelineLayoutCreateInfo blurLayoutInfo{};
        blurLayoutInfo.setLayoutCount = 1;
        blurLayoutInfo.pSetLayouts = &blurDescriptorLayout;
        blurLayout = device.createPipelineLayout(blurLayoutInfo);

        // Composite pipeline layout
        vk::PushConstantRange compositePush{};
        compositePush.stageFlags = vk::ShaderStageFlagBits::eFragment;
        compositePush.offset = 0;
        compositePush.size = sizeof(float) * 2; // bloomStrength, exposure

        vk::PipelineLayoutCreateInfo compositeLayoutInfo{};
        compositeLayoutInfo.setLayoutCount = 1;
        compositeLayoutInfo.pSetLayouts = &compositeDescriptorLayout;
        compositeLayoutInfo.pushConstantRangeCount = 1;
        compositeLayoutInfo.pPushConstantRanges = &compositePush;
        compositeLayout = device.createPipelineLayout(compositeLayoutInfo);

        // Bright pass pipeline
        PipelineBuilder brightBuilder(context, "shaders/bloom_blur.vert.spv", "shaders/bloom_brightpass.frag.spv");
        brightBuilder.pipelineLayout = brightPassLayout;
        brightBuilder.setColorAttachmentFormat(brightPassImage.imageFormat);
        brightBuilder.disableDepthTest();
        brightBuilder.disableBlending();
        brightBuilder.setMultisamplingNone();
        brightBuilder.setCullMode(vk::CullModeFlagBits::eNone, vk::FrontFace::eCounterClockwise);
        brightPassPipeline = brightBuilder.buildPipeline(device);

        // Blur vertical pipeline (specialization constant = 0)
        PipelineBuilder blurVBuilder(context, "shaders/bloom_blur.vert.spv", "shaders/bloom_blur.frag.spv");
        blurVBuilder.pipelineLayout = blurLayout;
        blurVBuilder.setColorAttachmentFormat(blurImageV.imageFormat);
        blurVBuilder.disableDepthTest();
        blurVBuilder.disableBlending();
        blurVBuilder.setMultisamplingNone();
        blurVBuilder.setCullMode(vk::CullModeFlagBits::eNone, vk::FrontFace::eCounterClockwise);

        // Add specialization constant for vertical blur (0)
        uint32_t vertDirection = 0;
        vk::SpecializationMapEntry specEntry{};
        specEntry.constantID = 0;
        specEntry.offset = 0;
        specEntry.size = sizeof(uint32_t);
        vk::SpecializationInfo specInfoVert{};
        specInfoVert.mapEntryCount = 1;
        specInfoVert.pMapEntries = &specEntry;
        specInfoVert.dataSize = sizeof(uint32_t);
        specInfoVert.pData = &vertDirection;
        blurVBuilder.setFragmentSpecialization(specInfoVert);
        blurVertPipeline = blurVBuilder.buildPipeline(device);

        // Blur horizontal pipeline (specialization constant = 1)
        uint32_t horzDirection = 1;
        vk::SpecializationInfo specInfoHorz{};
        specInfoHorz.mapEntryCount = 1;
        specInfoHorz.pMapEntries = &specEntry;
        specInfoHorz.dataSize = sizeof(uint32_t);
        specInfoHorz.pData = &horzDirection;

        PipelineBuilder blurHBuilder(context, "shaders/bloom_blur.vert.spv", "shaders/bloom_blur.frag.spv");
        blurHBuilder.pipelineLayout = blurLayout;
        blurHBuilder.setColorAttachmentFormat(blurImageH.imageFormat);
        blurHBuilder.disableDepthTest();
        blurHBuilder.disableBlending();
        blurHBuilder.setMultisamplingNone();
        blurHBuilder.setCullMode(vk::CullModeFlagBits::eNone, vk::FrontFace::eCounterClockwise);
        blurHBuilder.setFragmentSpecialization(specInfoHorz);
        blurHorzPipeline = blurHBuilder.buildPipeline(device);

        // Composite pipeline (additive blending)
        PipelineBuilder compositeBuilder(context, "shaders/bloom_blur.vert.spv", "shaders/bloom_composite.frag.spv");
        compositeBuilder.pipelineLayout = compositeLayout;
        compositeBuilder.setColorAttachmentFormat(context->getDrawImage().imageFormat);
        compositeBuilder.disableDepthTest();
        compositeBuilder.disableBlending();
        compositeBuilder.setMultisamplingNone();
        compositeBuilder.setCullMode(vk::CullModeFlagBits::eNone, vk::FrontFace::eCounterClockwise);
        compositePipeline = compositeBuilder.buildPipeline(device);
    }

    void BloomTechnique::apply(vk::CommandBuffer cmd, Image& inputImage, Image& outputImage,
                               DescriptorAllocatorGrowable& frameDescriptors) {
        vk::Device device = context->getDevice();

        if (!params.enabled) {
            // Just render input to output with composite (no bloom)
            vk::RenderingAttachmentInfo compositeAttachment = attachmentInfo(
                outputImage.imageView, nullptr, vk::ImageLayout::eColorAttachmentOptimal);

            vk::RenderingInfo compositeRenderInfo{};
            compositeRenderInfo.renderArea = vk::Rect2D({0, 0}, {width, height});
            compositeRenderInfo.layerCount = 1;
            compositeRenderInfo.colorAttachmentCount = 1;
            compositeRenderInfo.pColorAttachments = &compositeAttachment;

            cmd.beginRendering(&compositeRenderInfo);

            vk::Viewport fullViewport(0, 0, (float)width, (float)height, 0, 1);
            cmd.setViewport(0, 1, &fullViewport);
            vk::Rect2D fullScissor({0, 0}, {width, height});
            cmd.setScissor(0, 1, &fullScissor);

            cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, compositePipeline->getPipeline());

            vk::DescriptorSet compositeDescriptor = frameDescriptors.allocate(compositeDescriptorLayout);
            DescriptorWriter compositeWriter;
            compositeWriter.writeImage(0, inputImage.imageView, bloomSampler,
                vk::ImageLayout::eShaderReadOnlyOptimal, vk::DescriptorType::eCombinedImageSampler);
            compositeWriter.writeImage(1, inputImage.imageView, bloomSampler,  // Use same image, bloom strength will be 0
                vk::ImageLayout::eShaderReadOnlyOptimal, vk::DescriptorType::eCombinedImageSampler);
            compositeWriter.updateSet(device, compositeDescriptor);
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, compositeLayout, 0, 1, &compositeDescriptor, 0, nullptr);

            // Push constants: bloom strength = 0, exposure = 1
            float compositePushData[2] = { 0.0f, 1.0f };
            cmd.pushConstants(compositeLayout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(compositePushData), compositePushData);

            cmd.draw(3, 1, 0, 0);
            cmd.endRendering();
            return;
        }

        uint32_t bloomWidth = width / 2;
        uint32_t bloomHeight = height / 2;

        // Update blur params
        float* blurData = static_cast<float*>(blurParamsBuffer.info.pMappedData);
        blurData[0] = params.blurScale;
        blurData[1] = params.blurStrength;

        // 1. Bright pass: Extract bright areas from input
        transitionImage(cmd, brightPassImage.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal);

        vk::RenderingAttachmentInfo brightAttachment = attachmentInfo(
            brightPassImage.imageView, nullptr, vk::ImageLayout::eColorAttachmentOptimal);
        vk::ClearValue clearBlack{};
        clearBlack.color = vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f});
        brightAttachment.clearValue = clearBlack;
        brightAttachment.loadOp = vk::AttachmentLoadOp::eClear;

        vk::RenderingInfo brightRenderInfo{};
        brightRenderInfo.renderArea = vk::Rect2D({0, 0}, {bloomWidth, bloomHeight});
        brightRenderInfo.layerCount = 1;
        brightRenderInfo.colorAttachmentCount = 1;
        brightRenderInfo.pColorAttachments = &brightAttachment;

        cmd.beginRendering(&brightRenderInfo);

        vk::Viewport viewport(0, 0, (float)bloomWidth, (float)bloomHeight, 0, 1);
        cmd.setViewport(0, 1, &viewport);
        vk::Rect2D scissor({0, 0}, {bloomWidth, bloomHeight});
        cmd.setScissor(0, 1, &scissor);

        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, brightPassPipeline->getPipeline());

        // Allocate descriptor for input image
        vk::DescriptorSet brightDescriptor = frameDescriptors.allocate(singleImageLayout);
        DescriptorWriter brightWriter;
        brightWriter.writeImage(0, inputImage.imageView, bloomSampler,
            vk::ImageLayout::eShaderReadOnlyOptimal, vk::DescriptorType::eCombinedImageSampler);
        brightWriter.updateSet(device, brightDescriptor);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, brightPassLayout, 0, 1, &brightDescriptor, 0, nullptr);

        // Push constants for bright pass
        float brightPushData[2] = { params.threshold, params.intensity };
        cmd.pushConstants(brightPassLayout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(brightPushData), brightPushData);

        cmd.draw(3, 1, 0, 0);
        cmd.endRendering();

        // Transition bright pass result for reading
        transitionImage(cmd, brightPassImage.image, vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::eShaderReadOnlyOptimal);

        // 2. Vertical blur: brightPassImage -> blurImageV
        transitionImage(cmd, blurImageV.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal);

        vk::RenderingAttachmentInfo blurVAttachment = attachmentInfo(
            blurImageV.imageView, &clearBlack, vk::ImageLayout::eColorAttachmentOptimal);

        vk::RenderingInfo blurVRenderInfo{};
        blurVRenderInfo.renderArea = vk::Rect2D({0, 0}, {bloomWidth, bloomHeight});
        blurVRenderInfo.layerCount = 1;
        blurVRenderInfo.colorAttachmentCount = 1;
        blurVRenderInfo.pColorAttachments = &blurVAttachment;

        cmd.beginRendering(&blurVRenderInfo);
        cmd.setViewport(0, 1, &viewport);
        cmd.setScissor(0, 1, &scissor);

        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, blurVertPipeline->getPipeline());

        vk::DescriptorSet blurVDescriptor = frameDescriptors.allocate(blurDescriptorLayout);
        DescriptorWriter blurVWriter;
        blurVWriter.writeImage(0, brightPassImage.imageView, bloomSampler,
            vk::ImageLayout::eShaderReadOnlyOptimal, vk::DescriptorType::eCombinedImageSampler);
        blurVWriter.writeBuffer(1, blurParamsBuffer.buffer, sizeof(float) * 2, 0, vk::DescriptorType::eUniformBuffer);
        blurVWriter.updateSet(device, blurVDescriptor);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, blurLayout, 0, 1, &blurVDescriptor, 0, nullptr);

        cmd.draw(3, 1, 0, 0);
        cmd.endRendering();

        transitionImage(cmd, blurImageV.image, vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::eShaderReadOnlyOptimal);

        // 3. Horizontal blur: blurImageV -> blurImageH
        transitionImage(cmd, blurImageH.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal);

        vk::RenderingAttachmentInfo blurHAttachment = attachmentInfo(
            blurImageH.imageView, &clearBlack, vk::ImageLayout::eColorAttachmentOptimal);

        vk::RenderingInfo blurHRenderInfo{};
        blurHRenderInfo.renderArea = vk::Rect2D({0, 0}, {bloomWidth, bloomHeight});
        blurHRenderInfo.layerCount = 1;
        blurHRenderInfo.colorAttachmentCount = 1;
        blurHRenderInfo.pColorAttachments = &blurHAttachment;

        cmd.beginRendering(&blurHRenderInfo);
        cmd.setViewport(0, 1, &viewport);
        cmd.setScissor(0, 1, &scissor);

        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, blurHorzPipeline->getPipeline());

        vk::DescriptorSet blurHDescriptor = frameDescriptors.allocate(blurDescriptorLayout);
        DescriptorWriter blurHWriter;
        blurHWriter.writeImage(0, blurImageV.imageView, bloomSampler,
            vk::ImageLayout::eShaderReadOnlyOptimal, vk::DescriptorType::eCombinedImageSampler);
        blurHWriter.writeBuffer(1, blurParamsBuffer.buffer, sizeof(float) * 2, 0, vk::DescriptorType::eUniformBuffer);
        blurHWriter.updateSet(device, blurHDescriptor);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, blurLayout, 0, 1, &blurHDescriptor, 0, nullptr);

        cmd.draw(3, 1, 0, 0);
        cmd.endRendering();

        transitionImage(cmd, blurImageH.image, vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::eShaderReadOnlyOptimal);

        // 4. Composite: Combine original scene with bloom
        vk::RenderingAttachmentInfo compositeAttachment = attachmentInfo(
            outputImage.imageView, nullptr, vk::ImageLayout::eColorAttachmentOptimal);

        vk::RenderingInfo compositeRenderInfo{};
        compositeRenderInfo.renderArea = vk::Rect2D({0, 0}, {width, height});
        compositeRenderInfo.layerCount = 1;
        compositeRenderInfo.colorAttachmentCount = 1;
        compositeRenderInfo.pColorAttachments = &compositeAttachment;

        cmd.beginRendering(&compositeRenderInfo);

        vk::Viewport fullViewport(0, 0, (float)width, (float)height, 0, 1);
        cmd.setViewport(0, 1, &fullViewport);
        vk::Rect2D fullScissor({0, 0}, {width, height});
        cmd.setScissor(0, 1, &fullScissor);

        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, compositePipeline->getPipeline());

        vk::DescriptorSet compositeDescriptor = frameDescriptors.allocate(compositeDescriptorLayout);
        DescriptorWriter compositeWriter;
        compositeWriter.writeImage(0, inputImage.imageView, bloomSampler,
            vk::ImageLayout::eShaderReadOnlyOptimal, vk::DescriptorType::eCombinedImageSampler);
        compositeWriter.writeImage(1, blurImageH.imageView, bloomSampler,
            vk::ImageLayout::eShaderReadOnlyOptimal, vk::DescriptorType::eCombinedImageSampler);
        compositeWriter.updateSet(device, compositeDescriptor);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, compositeLayout, 0, 1, &compositeDescriptor, 0, nullptr);

        // Push constants for composite
        float compositePushData[2] = { params.bloomStrength, params.exposure };
        cmd.pushConstants(compositeLayout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(compositePushData), compositePushData);

        cmd.draw(3, 1, 0, 0);
        cmd.endRendering();
    }

} // namespace graphics::techniques
