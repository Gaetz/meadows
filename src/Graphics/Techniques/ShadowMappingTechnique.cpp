#include "ShadowMappingTechnique.h"

#include "../Renderer.h"
#include "../DescriptorLayoutBuilder.hpp"
#include "../DescriptorWriter.h"
#include "../PipelineBuilder.h"
#include "../Utils.hpp"
#include "../VulkanInit.hpp"
#include "BasicServices/File.h"
#include "BasicServices/RenderingStats.h"

namespace graphics::techniques {

    namespace {
        bool isVisible(const RenderObject& obj, const Mat4& viewProj) {
            std::array<Vec3, 8> corners {
                Vec3 { 1, 1, 1 },
                Vec3 { 1, 1, -1 },
                Vec3 { 1, -1, 1 },
                Vec3 { 1, -1, -1 },
                Vec3 { -1, 1, 1 },
                Vec3 { -1, 1, -1 },
                Vec3 { -1, -1, 1 },
                Vec3 { -1, -1, -1 },
            };

            Mat4 matrix = viewProj * obj.transform;

            Vec3 min = { 1.5f, 1.5f, 1.5f };
            Vec3 max = { -1.5f, -1.5f, -1.5f };

            for (int c = 0; c < 8; c++) {
                Vec4 v = matrix * Vec4(obj.bounds.origin + (corners[c] * obj.bounds.extents), 1.f);

                v.x = v.x / v.w;
                v.y = v.y / v.w;
                v.z = v.z / v.w;

                min = glm::min(Vec3{ v.x, v.y, v.z }, min);
                max = glm::max(Vec3{ v.x, v.y, v.z }, max);
            }

            if (min.z > 1.f || max.z < 0.f || min.x > 1.f || max.x < -1.f || min.y > 1.f || max.y < -1.f) {
                return false;
            }
            return true;
        }
    }

    void ShadowMappingTechnique::init(Renderer* renderer) {
        this->renderer = renderer;
        vk::Device device = renderer->getContext()->getDevice();

        shadowMap = std::make_unique<ShadowMap>(renderer->getContext(), 2048);

        DescriptorLayoutBuilder shadowBuilder;
        shadowBuilder.addBinding(0, vk::DescriptorType::eUniformBuffer);
        shadowBuilder.addBinding(1, vk::DescriptorType::eCombinedImageSampler);
        shadowSceneDataLayout = shadowBuilder.build(device, vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment);

        buildDepthPipeline(device);
        buildShadowMeshPipeline(device);
        buildDebugPipeline(device);
    }

    void ShadowMappingTechnique::cleanup(vk::Device device) {
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
        if (shadowSceneDataLayout) {
            device.destroyDescriptorSetLayout(shadowSceneDataLayout);
            shadowSceneDataLayout = nullptr;
        }

        if (shadowMap) {
            shadowMap->destroy();
            shadowMap.reset();
        }

        depthPipeline.reset();
        shadowMeshPipeline.reset();
        debugPipeline.reset();
    }

    void ShadowMappingTechnique::buildDepthPipeline(vk::Device device) {
        vk::PushConstantRange matrixRange{};
        matrixRange.offset = 0;
        matrixRange.size = sizeof(GraphicsPushConstants);
        matrixRange.stageFlags = vk::ShaderStageFlagBits::eVertex;

        const vk::DescriptorSetLayout layouts[] = { renderer->getSceneDataDescriptorLayout() };

        vk::PipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = layouts;
        layoutInfo.pPushConstantRanges = &matrixRange;
        layoutInfo.pushConstantRangeCount = 1;

        depthPipelineLayout = device.createPipelineLayout(layoutInfo);

        PipelineBuilder builder(renderer->getContext());

        vk::ShaderModule vertShader = graphics::createShaderModule(
            services::File::readBinary("shaders/shadowDepth.vert.spv"),
            device
        );
        builder.setVertexShaderOnly(vertShader);

        builder.setInputTopology(vk::PrimitiveTopology::eTriangleList);
        builder.setPolygonMode(vk::PolygonMode::eFill);
        builder.setCullMode(vk::CullModeFlagBits::eNone, vk::FrontFace::eClockwise);
        builder.setMultisamplingNone();
        builder.disableBlending();
        builder.enableDepthTest(true, vk::CompareOp::eLessOrEqual);
        builder.setDepthBiasEnable(true);
        builder.setDepthOnlyMode(true);
        builder.setDepthFormat(vk::Format::eD16Unorm);

        builder.pipelineLayout = depthPipelineLayout;

        depthPipeline = builder.buildPipeline(device);

        device.destroyShaderModule(vertShader);
    }

    void ShadowMappingTechnique::buildShadowMeshPipeline(vk::Device device) {
        vk::PushConstantRange matrixRange{};
        matrixRange.offset = 0;
        matrixRange.size = sizeof(GraphicsPushConstants);
        matrixRange.stageFlags = vk::ShaderStageFlagBits::eVertex;

        DescriptorLayoutBuilder layoutBuilder;
        layoutBuilder.addBinding(0, vk::DescriptorType::eUniformBuffer);
        layoutBuilder.addBinding(1, vk::DescriptorType::eCombinedImageSampler);
        layoutBuilder.addBinding(2, vk::DescriptorType::eCombinedImageSampler);

        materialLayout = layoutBuilder.build(
            device, vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment);

        const vk::DescriptorSetLayout layouts[] = {
            shadowSceneDataLayout,
            materialLayout
        };

        vk::PipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.setLayoutCount = 2;
        layoutInfo.pSetLayouts = layouts;
        layoutInfo.pPushConstantRanges = &matrixRange;
        layoutInfo.pushConstantRangeCount = 1;

        shadowMeshPipelineLayout = device.createPipelineLayout(layoutInfo);

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

    void ShadowMappingTechnique::buildDebugPipeline(vk::Device device) {
        const vk::DescriptorSetLayout layouts[] = { shadowSceneDataLayout };

        vk::PipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = layouts;
        layoutInfo.pPushConstantRanges = nullptr;
        layoutInfo.pushConstantRangeCount = 0;

        debugPipelineLayout = device.createPipelineLayout(layoutInfo);

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

    void ShadowMappingTechnique::render(
        vk::CommandBuffer cmd,
        const DrawContext& drawContext,
        const GPUSceneData& sceneData,
        DescriptorAllocatorGrowable& frameDescriptors
    ) {
        auto& stats = services::RenderingStats::Instance();
        stats.drawcallCount = 0;
        stats.triangleCount = 0;

        auto start = std::chrono::system_clock::now();

        renderShadowPass(cmd, drawContext, sceneData, frameDescriptors);

        Image& drawImage = renderer->getContext()->getDrawImage();
        Image& depthImage = renderer->getContext()->getDepthImage();

        vk::RenderingAttachmentInfo colorAttachment = graphics::attachmentInfo(
            drawImage.imageView, nullptr, vk::ImageLayout::eColorAttachmentOptimal);
        vk::RenderingAttachmentInfo depthAttachment = graphics::depthAttachmentInfo(
            depthImage.imageView, vk::ImageLayout::eDepthAttachmentOptimal);

        const auto imageExtent = drawImage.imageExtent;
        const vk::Rect2D rect{ vk::Offset2D{0, 0}, {imageExtent.width, imageExtent.height} };
        const vk::RenderingInfo renderInfo = graphics::renderingInfo(rect, &colorAttachment, &depthAttachment);
        cmd.beginRendering(&renderInfo);

        vk::DescriptorSet shadowSceneDescriptor = frameDescriptors.allocate(shadowSceneDataLayout);
        {
            DescriptorWriter writer;
            writer.writeBuffer(0, renderer->getSceneDataBuffer().buffer, sizeof(GPUSceneData), 0, vk::DescriptorType::eUniformBuffer);
            writer.writeImage(1, shadowMap->getImage().imageView, shadowMap->getSampler(),
                vk::ImageLayout::eShaderReadOnlyOptimal, vk::DescriptorType::eCombinedImageSampler);
            writer.updateSet(renderer->getContext()->getDevice(), shadowSceneDescriptor);
        }

        if (displayShadowMap) {
            renderDebugView(cmd, shadowSceneDescriptor);
        } else {
            renderShadowGeometry(cmd, drawContext, sceneData, shadowSceneDescriptor);
        }

        cmd.endRendering();

        auto end = std::chrono::system_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        stats.meshDrawTime = elapsed.count() / 1000.f;
    }

    void ShadowMappingTechnique::renderShadowPass(
        vk::CommandBuffer cmd,
        const DrawContext& drawContext,
        const GPUSceneData& sceneData,
        DescriptorAllocatorGrowable& frameDescriptors
    ) {
        graphics::transitionImage(cmd, shadowMap->getImage().image,
            vk::ImageLayout::eUndefined, vk::ImageLayout::eDepthAttachmentOptimal);

        vk::RenderingAttachmentInfo depthAttachment{};
        depthAttachment.imageView = shadowMap->getImage().imageView;
        depthAttachment.imageLayout = vk::ImageLayout::eDepthAttachmentOptimal;
        depthAttachment.loadOp = vk::AttachmentLoadOp::eClear;
        depthAttachment.storeOp = vk::AttachmentStoreOp::eStore;
        depthAttachment.clearValue.depthStencil = vk::ClearDepthStencilValue{1.0f, 0};

        uint32_t resolution = shadowMap->getResolution();
        vk::RenderingInfo renderInfo{};
        renderInfo.renderArea = vk::Rect2D{{0, 0}, {resolution, resolution}};
        renderInfo.layerCount = 1;
        renderInfo.pDepthAttachment = &depthAttachment;

        cmd.beginRendering(&renderInfo);

        vk::Viewport viewport{};
        viewport.x = 0;
        viewport.y = 0;
        viewport.width = static_cast<float>(resolution);
        viewport.height = static_cast<float>(resolution);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        cmd.setViewport(0, 1, &viewport);

        vk::Rect2D scissor{{0, 0}, {resolution, resolution}};
        cmd.setScissor(0, 1, &scissor);

        cmd.setDepthBias(
            shadowMap->getDepthBiasConstant(),
            0.0f,
            shadowMap->getDepthBiasSlope()
        );

        vk::DescriptorSet sceneDescriptor = frameDescriptors.allocate(renderer->getSceneDataDescriptorLayout());
        {
            DescriptorWriter writer;
            writer.writeBuffer(0, renderer->getSceneDataBuffer().buffer, sizeof(GPUSceneData), 0, vk::DescriptorType::eUniformBuffer);
            writer.updateSet(renderer->getContext()->getDevice(), sceneDescriptor);
        }

        depthPipeline->bind(cmd);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
            depthPipelineLayout, 0, 1, &sceneDescriptor, 0, nullptr);

        for (auto& r : drawContext.opaqueSurfaces) {
            if (isVisible(r, sceneData.lightSpaceMatrix)) {
                GraphicsPushConstants pushConstants{};
                pushConstants.vertexBuffer = r.vertexBufferAddress;
                pushConstants.worldMatrix = r.transform;

                cmd.pushConstants(depthPipelineLayout,
                    vk::ShaderStageFlagBits::eVertex, 0, sizeof(GraphicsPushConstants), &pushConstants);

                cmd.bindIndexBuffer(r.indexBuffer, 0, vk::IndexType::eUint32);
                cmd.drawIndexed(r.indexCount, 1, r.firstIndex, 0, 0);
            }
        }

        cmd.endRendering();

        graphics::transitionImage(cmd, shadowMap->getImage().image,
            vk::ImageLayout::eDepthAttachmentOptimal, vk::ImageLayout::eShaderReadOnlyOptimal);
    }

    void ShadowMappingTechnique::renderShadowGeometry(
        vk::CommandBuffer cmd,
        const DrawContext& drawContext,
        const GPUSceneData& sceneData,
        vk::DescriptorSet sceneDescriptor
    ) {
        auto& stats = services::RenderingStats::Instance();

        lastMaterial = nullptr;
        lastIndexBuffer = nullptr;

        std::vector<u32> opaqueDraws;
        opaqueDraws.reserve(drawContext.opaqueSurfaces.size());
        for (uint32_t i = 0; i < drawContext.opaqueSurfaces.size(); i++) {
            if (isVisible(drawContext.opaqueSurfaces[i], sceneData.viewProj)) {
                opaqueDraws.push_back(i);
            }
        }

        std::ranges::sort(opaqueDraws, [&](const auto& iA, const auto& iB) {
            const RenderObject& A = drawContext.opaqueSurfaces[iA];
            const RenderObject& B = drawContext.opaqueSurfaces[iB];
            if (A.material == B.material) {
                return A.indexBuffer < B.indexBuffer;
            }
            return A.material < B.material;
        });

        shadowMeshPipeline->bind(cmd);

        const auto imageExtent = renderer->getContext()->getDrawImage().imageExtent;
        vk::Viewport viewport{};
        viewport.x = 0;
        viewport.y = 0;
        viewport.width = static_cast<float>(imageExtent.width);
        viewport.height = static_cast<float>(imageExtent.height);
        viewport.minDepth = 0.f;
        viewport.maxDepth = 1.f;
        cmd.setViewport(0, 1, &viewport);

        vk::Rect2D scissor{{0, 0}, {imageExtent.width, imageExtent.height}};
        cmd.setScissor(0, 1, &scissor);

        for (auto& idx : opaqueDraws) {
            const RenderObject& r = drawContext.opaqueSurfaces[idx];

            if (r.material != lastMaterial) {
                lastMaterial = r.material;
                cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                    shadowMeshPipelineLayout, 0, 1, &sceneDescriptor, 0, nullptr);
                cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                    shadowMeshPipelineLayout, 1, 1, &r.material->materialSet, 0, nullptr);
            }

            if (r.indexBuffer != lastIndexBuffer) {
                lastIndexBuffer = r.indexBuffer;
                cmd.bindIndexBuffer(r.indexBuffer, 0, vk::IndexType::eUint32);
            }

            GraphicsPushConstants pushConstants{};
            pushConstants.vertexBuffer = r.vertexBufferAddress;
            pushConstants.worldMatrix = r.transform;
            cmd.pushConstants(shadowMeshPipelineLayout,
                vk::ShaderStageFlagBits::eVertex, 0, sizeof(GraphicsPushConstants), &pushConstants);

            cmd.drawIndexed(r.indexCount, 1, r.firstIndex, 0, 0);

            stats.drawcallCount++;
            stats.triangleCount += r.indexCount / 3;
        }
    }

    void ShadowMappingTechnique::renderDebugView(vk::CommandBuffer cmd, vk::DescriptorSet sceneDescriptor) {
        debugPipeline->bind(cmd);

        const auto imageExtent = renderer->getContext()->getDrawImage().imageExtent;
        vk::Viewport viewport{};
        viewport.x = 0;
        viewport.y = 0;
        viewport.width = static_cast<float>(imageExtent.width);
        viewport.height = static_cast<float>(imageExtent.height);
        viewport.minDepth = 0.f;
        viewport.maxDepth = 1.f;
        cmd.setViewport(0, 1, &viewport);

        vk::Rect2D scissor{{0, 0}, {imageExtent.width, imageExtent.height}};
        cmd.setScissor(0, 1, &scissor);

        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
            debugPipelineLayout, 0, 1, &sceneDescriptor, 0, nullptr);

        cmd.draw(3, 1, 0, 0);
    }

} // namespace graphics::techniques
