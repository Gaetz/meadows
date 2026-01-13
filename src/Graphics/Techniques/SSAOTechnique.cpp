#include "SSAOTechnique.h"
#include "../Renderer.h"
#include "../VulkanContext.h"
#include "../PipelineBuilder.h"
#include "../DescriptorLayoutBuilder.hpp"
#include "../DescriptorWriter.h"
#include "../Utils.hpp"
#include "../VulkanInit.hpp"
#include "BasicServices/Log.h"

#include <random>
#include <glm/glm.hpp>

using services::Log;

namespace graphics::techniques {

    // UBO structure for SSAO parameters (must match shader)
    struct SSAOParamsUBO {
        Mat4 projection;
        Mat4 view;
        float radius;
        float bias;
        float intensity;
        int32_t kernelSize;
    };

    void SSAOTechnique::init(Renderer* renderer, uint32_t width, uint32_t height) {
        this->renderer = renderer;
        this->context = renderer->getContext();
        this->width = width;
        this->height = height;

        // Create sampler for SSAO textures (clamp to edge)
        vk::SamplerCreateInfo samplerInfo{};
        samplerInfo.magFilter = vk::Filter::eNearest;
        samplerInfo.minFilter = vk::Filter::eNearest;
        samplerInfo.mipmapMode = vk::SamplerMipmapMode::eLinear;
        samplerInfo.addressModeU = vk::SamplerAddressMode::eClampToEdge;
        samplerInfo.addressModeV = vk::SamplerAddressMode::eClampToEdge;
        samplerInfo.addressModeW = vk::SamplerAddressMode::eClampToEdge;
        samplerInfo.maxLod = 1.0f;
        ssaoSampler = context->getDevice().createSampler(samplerInfo);

        // Create sampler for noise texture (repeat)
        vk::SamplerCreateInfo noiseSamplerInfo{};
        noiseSamplerInfo.magFilter = vk::Filter::eNearest;
        noiseSamplerInfo.minFilter = vk::Filter::eNearest;
        noiseSamplerInfo.mipmapMode = vk::SamplerMipmapMode::eNearest;
        noiseSamplerInfo.addressModeU = vk::SamplerAddressMode::eRepeat;
        noiseSamplerInfo.addressModeV = vk::SamplerAddressMode::eRepeat;
        noiseSamplerInfo.addressModeW = vk::SamplerAddressMode::eRepeat;
        noiseSampler = context->getDevice().createSampler(noiseSamplerInfo);

        // Create SSAO params buffer
        ssaoParamsBuffer = Buffer(context, sizeof(SSAOParamsUBO),
            vk::BufferUsageFlagBits::eUniformBuffer, VMA_MEMORY_USAGE_CPU_TO_GPU);

        createImages();
        createNoiseTexture();
        createKernel();
        createDescriptors();
        createPipelines();

        Log::Info("SSAO technique initialized (%dx%d), kernel size: %d", width, height, SSAO_KERNEL_SIZE);
    }

    void SSAOTechnique::cleanup(vk::Device device) {
        destroyImages();
        noiseImage.destroy(context);
        kernelBuffer.destroy();
        ssaoParamsBuffer.destroy();

        if (ssaoSampler) {
            device.destroySampler(ssaoSampler);
            ssaoSampler = nullptr;
        }
        if (noiseSampler) {
            device.destroySampler(noiseSampler);
            noiseSampler = nullptr;
        }

        device.destroyDescriptorSetLayout(ssaoDescriptorLayout);
        device.destroyDescriptorSetLayout(blurDescriptorLayout);
        device.destroyDescriptorSetLayout(compositeDescriptorLayout);

        device.destroyPipelineLayout(ssaoLayout);
        device.destroyPipelineLayout(blurLayout);
        device.destroyPipelineLayout(compositeLayout);
    }

    void SSAOTechnique::resize(uint32_t newWidth, uint32_t newHeight) {
        if (width == newWidth && height == newHeight) return;

        width = newWidth;
        height = newHeight;

        destroyImages();
        createImages();
    }

    void SSAOTechnique::createImages() {
        vk::Extent3D extent = { width, height, 1 };

        // SSAO images are single channel (R8)
        vk::Format format = vk::Format::eR8Unorm;
        vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eColorAttachment |
                                    vk::ImageUsageFlagBits::eSampled;

        ssaoImage = Image(context, extent, format, usage);
        ssaoBlurImage = Image(context, extent, format, usage);
    }

    void SSAOTechnique::destroyImages() {
        ssaoImage.destroy(context);
        ssaoBlurImage.destroy(context);
    }

    void SSAOTechnique::createNoiseTexture() {
        // Generate random rotation vectors for the noise texture
        std::default_random_engine rndEngine(static_cast<unsigned>(time(nullptr)));
        std::uniform_real_distribution<float> rndDist(0.0f, 1.0f);

        std::vector<glm::vec4> noiseData(SSAO_NOISE_DIM * SSAO_NOISE_DIM);
        for (int i = 0; i < SSAO_NOISE_DIM * SSAO_NOISE_DIM; i++) {
            // Random rotation around z-axis (tangent space)
            noiseData[i] = glm::vec4(
                rndDist(rndEngine) * 2.0f - 1.0f,
                rndDist(rndEngine) * 2.0f - 1.0f,
                0.0f,
                0.0f
            );
        }

        // Create noise image
        vk::Extent3D noiseExtent = { SSAO_NOISE_DIM, SSAO_NOISE_DIM, 1 };
        vk::Format format = vk::Format::eR32G32B32A32Sfloat;
        vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eSampled |
                                    vk::ImageUsageFlagBits::eTransferDst;

        noiseImage = Image(context, *renderer->getImmediateSubmitter(),
                          noiseData.data(), noiseExtent, format, usage, false);
    }

    void SSAOTechnique::createKernel() {
        // Generate hemisphere sample kernel
        std::default_random_engine rndEngine(static_cast<unsigned>(time(nullptr)));
        std::uniform_real_distribution<float> rndDist(0.0f, 1.0f);

        std::vector<glm::vec4> kernel(SSAO_KERNEL_SIZE);

        for (int i = 0; i < SSAO_KERNEL_SIZE; i++) {
            // Random point in hemisphere (tangent space, z up)
            glm::vec3 sample(
                rndDist(rndEngine) * 2.0f - 1.0f,
                rndDist(rndEngine) * 2.0f - 1.0f,
                rndDist(rndEngine)
            );
            sample = glm::normalize(sample);
            sample *= rndDist(rndEngine);

            // Scale samples so they're more aligned to center
            float scale = static_cast<float>(i) / static_cast<float>(SSAO_KERNEL_SIZE);
            scale = 0.1f + scale * scale * (1.0f - 0.1f); // lerp(0.1f, 1.0f, scale * scale)
            sample *= scale;

            kernel[i] = glm::vec4(sample, 0.0f);
        }

        // Create kernel buffer
        kernelBuffer = Buffer(context, kernel.size() * sizeof(glm::vec4),
            vk::BufferUsageFlagBits::eUniformBuffer, VMA_MEMORY_USAGE_CPU_TO_GPU);

        // Upload kernel data
        void* data;
        kernelBuffer.map(&data);
        memcpy(data, kernel.data(), kernel.size() * sizeof(glm::vec4));
        kernelBuffer.unmap();
    }

    void SSAOTechnique::createDescriptors() {
        vk::Device device = context->getDevice();

        // SSAO descriptor layout:
        // 0 - Position (sampler2D)
        // 1 - Normal (sampler2D)
        // 2 - Noise texture (sampler2D)
        // 3 - Kernel buffer (UBO)
        // 4 - SSAO params (UBO)
        DescriptorLayoutBuilder ssaoBuilder;
        ssaoBuilder.addBinding(0, vk::DescriptorType::eCombinedImageSampler);
        ssaoBuilder.addBinding(1, vk::DescriptorType::eCombinedImageSampler);
        ssaoBuilder.addBinding(2, vk::DescriptorType::eCombinedImageSampler);
        ssaoBuilder.addBinding(3, vk::DescriptorType::eUniformBuffer);
        ssaoBuilder.addBinding(4, vk::DescriptorType::eUniformBuffer);
        ssaoDescriptorLayout = ssaoBuilder.build(device, vk::ShaderStageFlagBits::eFragment);

        // Blur descriptor layout:
        // 0 - SSAO input (sampler2D)
        DescriptorLayoutBuilder blurBuilder;
        blurBuilder.addBinding(0, vk::DescriptorType::eCombinedImageSampler);
        blurDescriptorLayout = blurBuilder.build(device, vk::ShaderStageFlagBits::eFragment);

        // Composite descriptor layout:
        // 0 - Scene color (sampler2D)
        // 1 - SSAO (sampler2D)
        DescriptorLayoutBuilder compositeBuilder;
        compositeBuilder.addBinding(0, vk::DescriptorType::eCombinedImageSampler);
        compositeBuilder.addBinding(1, vk::DescriptorType::eCombinedImageSampler);
        compositeDescriptorLayout = compositeBuilder.build(device, vk::ShaderStageFlagBits::eFragment);
    }

    void SSAOTechnique::createPipelines() {
        vk::Device device = context->getDevice();

        // SSAO pipeline layout
        vk::PipelineLayoutCreateInfo ssaoLayoutInfo{};
        ssaoLayoutInfo.setLayoutCount = 1;
        ssaoLayoutInfo.pSetLayouts = &ssaoDescriptorLayout;
        ssaoLayout = device.createPipelineLayout(ssaoLayoutInfo);

        // Blur pipeline layout
        vk::PipelineLayoutCreateInfo blurLayoutInfo{};
        blurLayoutInfo.setLayoutCount = 1;
        blurLayoutInfo.pSetLayouts = &blurDescriptorLayout;
        blurLayout = device.createPipelineLayout(blurLayoutInfo);

        // Composite pipeline layout (with push constant for ssaoOnly flag)
        vk::PushConstantRange compositePush{};
        compositePush.stageFlags = vk::ShaderStageFlagBits::eFragment;
        compositePush.offset = 0;
        compositePush.size = sizeof(int32_t); // ssaoOnly flag

        vk::PipelineLayoutCreateInfo compositeLayoutInfo{};
        compositeLayoutInfo.setLayoutCount = 1;
        compositeLayoutInfo.pSetLayouts = &compositeDescriptorLayout;
        compositeLayoutInfo.pushConstantRangeCount = 1;
        compositeLayoutInfo.pPushConstantRanges = &compositePush;
        compositeLayout = device.createPipelineLayout(compositeLayoutInfo);

        // SSAO pipeline
        PipelineBuilder ssaoBuilder(context, "shaders/ssao.vert.spv", "shaders/ssao.frag.spv");
        ssaoBuilder.pipelineLayout = ssaoLayout;
        ssaoBuilder.setColorAttachmentFormat(ssaoImage.imageFormat);
        ssaoBuilder.disableDepthTest();
        ssaoBuilder.disableBlending();
        ssaoBuilder.setMultisamplingNone();
        ssaoBuilder.setCullMode(vk::CullModeFlagBits::eNone, vk::FrontFace::eCounterClockwise);
        ssaoPipeline = ssaoBuilder.buildPipeline(device);

        // Blur pipeline
        PipelineBuilder blurBuilder(context, "shaders/ssao.vert.spv", "shaders/ssao_blur.frag.spv");
        blurBuilder.pipelineLayout = blurLayout;
        blurBuilder.setColorAttachmentFormat(ssaoBlurImage.imageFormat);
        blurBuilder.disableDepthTest();
        blurBuilder.disableBlending();
        blurBuilder.setMultisamplingNone();
        blurBuilder.setCullMode(vk::CullModeFlagBits::eNone, vk::FrontFace::eCounterClockwise);
        blurPipeline = blurBuilder.buildPipeline(device);

        // Composite pipeline
        PipelineBuilder compositeBuilder(context, "shaders/ssao.vert.spv", "shaders/ssao_composite.frag.spv");
        compositeBuilder.pipelineLayout = compositeLayout;
        compositeBuilder.setColorAttachmentFormat(context->getDrawImage().imageFormat);
        compositeBuilder.disableDepthTest();
        compositeBuilder.disableBlending();
        compositeBuilder.setMultisamplingNone();
        compositeBuilder.setCullMode(vk::CullModeFlagBits::eNone, vk::FrontFace::eCounterClockwise);
        compositePipeline = compositeBuilder.buildPipeline(device);
    }

    void SSAOTechnique::apply(vk::CommandBuffer cmd,
                              Image& inputImage,
                              Image& outputImage,
                              Image& positionImage,
                              Image& normalImage,
                              const Mat4& projection,
                              const Mat4& view,
                              DescriptorAllocatorGrowable& frameDescriptors) {
        vk::Device device = context->getDevice();

        if (!params.enabled) {
            // If SSAO is disabled, just copy input to output
            transitionImage(cmd, inputImage.image, vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eTransferSrcOptimal);
            transitionImage(cmd, outputImage.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal);

            vk::ImageCopy copyRegion{};
            copyRegion.srcSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
            copyRegion.srcSubresource.layerCount = 1;
            copyRegion.dstSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
            copyRegion.dstSubresource.layerCount = 1;
            copyRegion.extent = inputImage.imageExtent;
            cmd.copyImage(inputImage.image, vk::ImageLayout::eTransferSrcOptimal,
                         outputImage.image, vk::ImageLayout::eTransferDstOptimal, 1, &copyRegion);

            transitionImage(cmd, inputImage.image, vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eShaderReadOnlyOptimal);
            transitionImage(cmd, outputImage.image, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eColorAttachmentOptimal);
            return;
        }

        // Update SSAO params
        SSAOParamsUBO paramsUBO;
        paramsUBO.projection = projection;
        paramsUBO.view = view;
        paramsUBO.radius = params.radius;
        paramsUBO.bias = params.bias;
        paramsUBO.intensity = params.intensity;
        paramsUBO.kernelSize = SSAO_KERNEL_SIZE;

        void* data;
        ssaoParamsBuffer.map(&data);
        memcpy(data, &paramsUBO, sizeof(SSAOParamsUBO));
        ssaoParamsBuffer.unmap();

        // 1. SSAO Pass - Generate SSAO from G-Buffer
        transitionImage(cmd, ssaoImage.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal);

        vk::ClearValue clearBlack{};
        clearBlack.color = vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f});

        vk::RenderingAttachmentInfo ssaoAttachment = attachmentInfo(
            ssaoImage.imageView, &clearBlack, vk::ImageLayout::eColorAttachmentOptimal);
        ssaoAttachment.loadOp = vk::AttachmentLoadOp::eClear;

        vk::RenderingInfo ssaoRenderInfo{};
        ssaoRenderInfo.renderArea = vk::Rect2D({0, 0}, {width, height});
        ssaoRenderInfo.layerCount = 1;
        ssaoRenderInfo.colorAttachmentCount = 1;
        ssaoRenderInfo.pColorAttachments = &ssaoAttachment;

        cmd.beginRendering(&ssaoRenderInfo);

        vk::Viewport viewport(0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1);
        cmd.setViewport(0, 1, &viewport);
        vk::Rect2D scissor({0, 0}, {width, height});
        cmd.setScissor(0, 1, &scissor);

        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, ssaoPipeline->getPipeline());

        // Allocate and update SSAO descriptor set
        vk::DescriptorSet ssaoDescriptor = frameDescriptors.allocate(ssaoDescriptorLayout);
        DescriptorWriter ssaoWriter;
        ssaoWriter.writeImage(0, positionImage.imageView, ssaoSampler,
            vk::ImageLayout::eShaderReadOnlyOptimal, vk::DescriptorType::eCombinedImageSampler);
        ssaoWriter.writeImage(1, normalImage.imageView, ssaoSampler,
            vk::ImageLayout::eShaderReadOnlyOptimal, vk::DescriptorType::eCombinedImageSampler);
        ssaoWriter.writeImage(2, noiseImage.imageView, noiseSampler,
            vk::ImageLayout::eShaderReadOnlyOptimal, vk::DescriptorType::eCombinedImageSampler);
        ssaoWriter.writeBuffer(3, kernelBuffer.buffer, SSAO_KERNEL_SIZE * sizeof(glm::vec4), 0, vk::DescriptorType::eUniformBuffer);
        ssaoWriter.writeBuffer(4, ssaoParamsBuffer.buffer, sizeof(SSAOParamsUBO), 0, vk::DescriptorType::eUniformBuffer);
        ssaoWriter.updateSet(device, ssaoDescriptor);

        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, ssaoLayout, 0, 1, &ssaoDescriptor, 0, nullptr);

        cmd.draw(3, 1, 0, 0);
        cmd.endRendering();

        transitionImage(cmd, ssaoImage.image, vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::eShaderReadOnlyOptimal);

        // 2. Blur Pass (if enabled)
        Image* ssaoResult = &ssaoImage;
        if (params.blurEnabled) {
            transitionImage(cmd, ssaoBlurImage.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal);

            vk::RenderingAttachmentInfo blurAttachment = attachmentInfo(
                ssaoBlurImage.imageView, &clearBlack, vk::ImageLayout::eColorAttachmentOptimal);
            blurAttachment.loadOp = vk::AttachmentLoadOp::eClear;

            vk::RenderingInfo blurRenderInfo{};
            blurRenderInfo.renderArea = vk::Rect2D({0, 0}, {width, height});
            blurRenderInfo.layerCount = 1;
            blurRenderInfo.colorAttachmentCount = 1;
            blurRenderInfo.pColorAttachments = &blurAttachment;

            cmd.beginRendering(&blurRenderInfo);
            cmd.setViewport(0, 1, &viewport);
            cmd.setScissor(0, 1, &scissor);

            cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, blurPipeline->getPipeline());

            vk::DescriptorSet blurDescriptor = frameDescriptors.allocate(blurDescriptorLayout);
            DescriptorWriter blurWriter;
            blurWriter.writeImage(0, ssaoImage.imageView, ssaoSampler,
                vk::ImageLayout::eShaderReadOnlyOptimal, vk::DescriptorType::eCombinedImageSampler);
            blurWriter.updateSet(device, blurDescriptor);

            cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, blurLayout, 0, 1, &blurDescriptor, 0, nullptr);

            cmd.draw(3, 1, 0, 0);
            cmd.endRendering();

            transitionImage(cmd, ssaoBlurImage.image, vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::eShaderReadOnlyOptimal);
            ssaoResult = &ssaoBlurImage;
        }

        // 3. Composite Pass - Apply SSAO to scene color
        vk::RenderingAttachmentInfo compositeAttachment = attachmentInfo(
            outputImage.imageView, nullptr, vk::ImageLayout::eColorAttachmentOptimal);

        vk::RenderingInfo compositeRenderInfo{};
        compositeRenderInfo.renderArea = vk::Rect2D({0, 0}, {width, height});
        compositeRenderInfo.layerCount = 1;
        compositeRenderInfo.colorAttachmentCount = 1;
        compositeRenderInfo.pColorAttachments = &compositeAttachment;

        cmd.beginRendering(&compositeRenderInfo);
        cmd.setViewport(0, 1, &viewport);
        cmd.setScissor(0, 1, &scissor);

        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, compositePipeline->getPipeline());

        vk::DescriptorSet compositeDescriptor = frameDescriptors.allocate(compositeDescriptorLayout);
        DescriptorWriter compositeWriter;
        compositeWriter.writeImage(0, inputImage.imageView, ssaoSampler,
            vk::ImageLayout::eShaderReadOnlyOptimal, vk::DescriptorType::eCombinedImageSampler);
        compositeWriter.writeImage(1, ssaoResult->imageView, ssaoSampler,
            vk::ImageLayout::eShaderReadOnlyOptimal, vk::DescriptorType::eCombinedImageSampler);
        compositeWriter.updateSet(device, compositeDescriptor);

        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, compositeLayout, 0, 1, &compositeDescriptor, 0, nullptr);

        // Push constant for ssaoOnly flag
        int32_t ssaoOnly = params.ssaoOnly ? 1 : 0;
        cmd.pushConstants(compositeLayout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(int32_t), &ssaoOnly);

        cmd.draw(3, 1, 0, 0);
        cmd.endRendering();
    }

} // namespace graphics::techniques
