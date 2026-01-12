#include "BasicTechnique.h"

#include "../Renderer.h"
#include "../DescriptorLayoutBuilder.hpp"
#include "../DescriptorWriter.h"
#include "../PipelineBuilder.h"
#include "../Utils.hpp"
#include "../VulkanInit.hpp"
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

    void BasicTechnique::init(Renderer* renderer) {
        this->renderer = renderer;
        vk::Device device = renderer->getContext()->getDevice();

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

        const vk::DescriptorSetLayout layouts[] = { renderer->getSceneDataDescriptorLayout(), materialLayout };

        vk::PipelineLayoutCreateInfo meshLayoutInfo{};
        meshLayoutInfo.setLayoutCount = 2;
        meshLayoutInfo.pSetLayouts = layouts;
        meshLayoutInfo.pPushConstantRanges = &matrixRange;
        meshLayoutInfo.pushConstantRangeCount = 1;

        pipelineLayout = device.createPipelineLayout(meshLayoutInfo);

        PipelineBuilder pipelineBuilder{ renderer->getContext(), "shaders/mesh.vert.spv", "shaders/mesh.frag.spv" };
        pipelineBuilder.setInputTopology(vk::PrimitiveTopology::eTriangleList);
        pipelineBuilder.setPolygonMode(vk::PolygonMode::eFill);
        pipelineBuilder.setCullMode(vk::CullModeFlagBits::eNone, vk::FrontFace::eClockwise);
        pipelineBuilder.setMultisamplingNone();
        pipelineBuilder.disableBlending();
        pipelineBuilder.enableDepthTest(true, vk::CompareOp::eLessOrEqual);

        pipelineBuilder.setColorAttachmentFormat(renderer->getContext()->getDrawImage().imageFormat);
        pipelineBuilder.setDepthFormat(renderer->getContext()->getDepthImage().imageFormat);

        pipelineBuilder.pipelineLayout = pipelineLayout;

        opaquePipeline = pipelineBuilder.buildPipeline(device);

        pipelineBuilder.enableBlendingAdditive();
        pipelineBuilder.enableDepthTest(false, vk::CompareOp::eLessOrEqual);
        transparentPipeline = pipelineBuilder.buildPipeline(device);

        pipelineBuilder.destroyShaderModules(device);
    }

    void BasicTechnique::cleanup(vk::Device device) {
        if (pipelineLayout) {
            device.destroyPipelineLayout(pipelineLayout);
            pipelineLayout = nullptr;
        }
        if (materialLayout) {
            device.destroyDescriptorSetLayout(materialLayout);
            materialLayout = nullptr;
        }
        opaquePipeline.reset();
        transparentPipeline.reset();
    }

    void BasicTechnique::render(
        vk::CommandBuffer cmd,
        const DrawContext& drawContext,
        const GPUSceneData& sceneData,
        DescriptorAllocatorGrowable& frameDescriptors
    ) {
        auto& stats = services::RenderingStats::Instance();
        stats.drawcallCount = 0;
        stats.triangleCount = 0;

        auto start = std::chrono::system_clock::now();

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

        vk::DescriptorSet globalDescriptor = frameDescriptors.allocate(renderer->getSceneDataDescriptorLayout());
        {
            DescriptorWriter writer;
            writer.writeBuffer(0, renderer->getSceneDataBuffer().buffer, sizeof(GPUSceneData), 0, vk::DescriptorType::eUniformBuffer);
            writer.updateSet(renderer->getContext()->getDevice(), globalDescriptor);
        }

        renderGeometry(cmd, drawContext, sceneData, globalDescriptor);

        cmd.endRendering();

        auto end = std::chrono::system_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        stats.meshDrawTime = elapsed.count() / 1000.f;
    }

    void BasicTechnique::renderGeometry(
        vk::CommandBuffer cmd,
        const DrawContext& drawContext,
        const GPUSceneData& sceneData,
        vk::DescriptorSet globalDescriptor
    ) {
        auto& stats = services::RenderingStats::Instance();

        lastPipeline = nullptr;
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

        auto draw = [&](const RenderObject& r) {
            if (r.material != lastMaterial) {
                lastMaterial = r.material;
                if (r.material->pipeline != lastPipeline) {
                    lastPipeline = r.material->pipeline;
                    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, r.material->pipeline->getPipeline());
                    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, r.material->pipeline->getLayout(), 0, 1, &globalDescriptor, 0, nullptr);
                }
                cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, r.material->pipeline->getLayout(), 1, 1, &r.material->materialSet, 0, nullptr);
            }
            if (r.indexBuffer != lastIndexBuffer) {
                lastIndexBuffer = r.indexBuffer;
                cmd.bindIndexBuffer(r.indexBuffer, 0, vk::IndexType::eUint32);
            }

            GraphicsPushConstants pushConstants{};
            pushConstants.vertexBuffer = r.vertexBufferAddress;
            pushConstants.worldMatrix = r.transform;
            cmd.pushConstants(r.material->pipeline->getLayout(), vk::ShaderStageFlagBits::eVertex, 0, sizeof(GraphicsPushConstants), &pushConstants);

            cmd.drawIndexed(r.indexCount, 1, r.firstIndex, 0, 0);

            stats.drawcallCount++;
            stats.triangleCount += r.indexCount / 3;
        };

        for (auto& r : opaqueDraws) {
            draw(drawContext.opaqueSurfaces[r]);
        }

        for (auto& r : drawContext.transparentSurfaces) {
            if (isVisible(r, sceneData.viewProj)) {
                draw(r);
            }
        }
    }

} // namespace graphics::techniques
