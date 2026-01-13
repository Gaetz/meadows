/**
 * @file BasicTechnique.cpp
 * @brief Implementation of simple forward rendering without shadows.
 *
 * ## Forward Rendering Pipeline Overview
 *
 * This file implements the simplest rendering technique: forward rendering.
 * Each object is drawn once with full shading computed per-pixel.
 *
 * ## The Vulkan Rendering Commands Explained
 *
 * ### 1. Begin Rendering (Dynamic Rendering)
 * Vulkan 1.3 introduced "dynamic rendering" which simplifies render passes.
 * Instead of pre-creating VkRenderPass objects, we specify attachments inline:
 * - Color attachment: Where pixel colors are written
 * - Depth attachment: Where depth values are written (for depth testing)
 *
 * ### 2. Set Dynamic State
 * - **Viewport**: Maps normalized device coordinates to screen pixels
 * - **Scissor**: Clips rendering to a rectangular region
 *
 * ### 3. Bind Resources
 * - **Pipeline**: The compiled shader program + fixed-function state
 * - **Descriptor Sets**: Resources accessible to shaders (buffers, textures)
 * - **Index Buffer**: Indices for indexed drawing
 * - **Push Constants**: Small, fast-changing data (matrices)
 *
 * ### 4. Draw
 * - **drawIndexed()**: Draws triangles using indices from the index buffer
 *
 * ### 5. End Rendering
 * Finalizes the render pass, making the images ready for next use.
 */

#include "BasicTechnique.h"

#include "../Renderer.h"
#include "../DescriptorLayoutBuilder.hpp"
#include "../DescriptorWriter.h"
#include "../PipelineBuilder.h"
#include "../Utils.hpp"
#include "../VulkanInit.hpp"
#include "BasicServices/RenderingStats.h"

namespace graphics::techniques {

    // =========================================================================
    // Frustum Culling Helper
    // =========================================================================

    namespace {
        /**
         * @brief Tests if a render object is visible in the camera frustum.
         *
         * Frustum culling is a critical optimization that skips objects
         * outside the camera's view. This saves GPU work by not drawing
         * invisible geometry.
         *
         * How it works:
         * 1. Transform the object's bounding box corners to clip space
         * 2. Check if the box is completely outside any frustum plane
         * 3. If outside, the object is not visible
         *
         * @param obj The render object to test.
         * @param viewProj Combined view-projection matrix.
         * @return true if the object is potentially visible.
         */
        bool isVisible(const RenderObject& obj, const Mat4& viewProj) {
            // 8 corners of a unit cube
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

            // Transform from object space to clip space
            Mat4 matrix = viewProj * obj.transform;

            // Track min/max of transformed corners in clip space
            Vec3 min = { 1.5f, 1.5f, 1.5f };
            Vec3 max = { -1.5f, -1.5f, -1.5f };

            for (int c = 0; c < 8; c++) {
                // Transform corner to clip space
                Vec4 v = matrix * Vec4(obj.bounds.origin + (corners[c] * obj.bounds.extents), 1.f);

                // Perspective divide (clip space -> normalized device coordinates)
                v.x = v.x / v.w;
                v.y = v.y / v.w;
                v.z = v.z / v.w;

                min = glm::min(Vec3{ v.x, v.y, v.z }, min);
                max = glm::max(Vec3{ v.x, v.y, v.z }, max);
            }

            // Check against NDC bounds [-1, 1] for X/Y and [0, 1] for Z
            if (min.z > 1.f || max.z < 0.f || min.x > 1.f || max.x < -1.f || min.y > 1.f || max.y < -1.f) {
                return false;
            }
            return true;
        }
    }

    // =========================================================================
    // Initialization
    // =========================================================================

    void BasicTechnique::init(Renderer* renderer) {
        this->renderer = renderer;
        vk::Device device = renderer->getContext()->getDevice();

        // -----------------------------------------------------------------
        // Step 1: Define Push Constants
        // -----------------------------------------------------------------
        // Push constants are the fastest way to send small data to shaders.
        // We use them for per-object data: world matrix and vertex buffer address.
        vk::PushConstantRange matrixRange{};
        matrixRange.offset = 0;
        matrixRange.size = sizeof(GraphicsPushConstants);
        matrixRange.stageFlags = vk::ShaderStageFlagBits::eVertex;

        // -----------------------------------------------------------------
        // Step 2: Create Material Descriptor Set Layout
        // -----------------------------------------------------------------
        // Defines what resources each material provides to shaders:
        // - Binding 0: Uniform buffer (material constants like color, roughness)
        // - Binding 1: Base color texture
        // - Binding 2: Metallic-roughness texture
        DescriptorLayoutBuilder layoutBuilder;
        layoutBuilder.addBinding(0, vk::DescriptorType::eUniformBuffer);
        layoutBuilder.addBinding(1, vk::DescriptorType::eCombinedImageSampler);
        layoutBuilder.addBinding(2, vk::DescriptorType::eCombinedImageSampler);

        materialLayout = layoutBuilder.build(
            device, vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment);

        // -----------------------------------------------------------------
        // Step 3: Create Pipeline Layout
        // -----------------------------------------------------------------
        // The pipeline layout defines all resources accessible to the pipeline:
        // - Set 0: Scene data (camera matrices, lighting) - from Renderer
        // - Set 1: Material data (our materialLayout)
        // - Push constants: Per-object data (world matrix)
        const vk::DescriptorSetLayout layouts[] = { renderer->getSceneDataDescriptorLayout(), materialLayout };

        vk::PipelineLayoutCreateInfo meshLayoutInfo{};
        meshLayoutInfo.setLayoutCount = 2;
        meshLayoutInfo.pSetLayouts = layouts;
        meshLayoutInfo.pPushConstantRanges = &matrixRange;
        meshLayoutInfo.pushConstantRangeCount = 1;

        pipelineLayout = device.createPipelineLayout(meshLayoutInfo);

        // -----------------------------------------------------------------
        // Step 4: Create Opaque Pipeline
        // -----------------------------------------------------------------
        // For solid objects that completely block what's behind them.
        PipelineBuilder pipelineBuilder{ renderer->getContext(), "shaders/mesh.vert.spv", "shaders/mesh.frag.spv" };
        pipelineBuilder.setInputTopology(vk::PrimitiveTopology::eTriangleList);  // Triangles
        pipelineBuilder.setPolygonMode(vk::PolygonMode::eFill);                  // Solid fill
        pipelineBuilder.setCullMode(vk::CullModeFlagBits::eNone, vk::FrontFace::eClockwise);  // No culling for now
        pipelineBuilder.setMultisamplingNone();                                   // No MSAA
        pipelineBuilder.disableBlending();                                        // Opaque = no blending
        pipelineBuilder.enableDepthTest(true, vk::CompareOp::eLessOrEqual);      // Write to depth buffer

        // Set output format to match our render target
        pipelineBuilder.setColorAttachmentFormat(renderer->getSceneImage().imageFormat);
        pipelineBuilder.setDepthFormat(renderer->getContext()->getDepthImage().imageFormat);

        pipelineBuilder.pipelineLayout = pipelineLayout;

        opaquePipeline = pipelineBuilder.buildPipeline(device);

        // -----------------------------------------------------------------
        // Step 5: Create Transparent Pipeline
        // -----------------------------------------------------------------
        // For see-through objects. Key differences:
        // - Additive blending: new color adds to existing
        // - No depth write: transparent objects don't occlude each other
        pipelineBuilder.enableBlendingAdditive();
        pipelineBuilder.enableDepthTest(false, vk::CompareOp::eLessOrEqual);  // Read depth, don't write
        transparentPipeline = pipelineBuilder.buildPipeline(device);

        // Clean up shader modules - they're baked into the pipelines
        pipelineBuilder.destroyShaderModules(device);
    }

    // =========================================================================
    // Cleanup
    // =========================================================================

    void BasicTechnique::cleanup(vk::Device device) {
        // Destroy in reverse order of creation
        if (pipelineLayout) {
            device.destroyPipelineLayout(pipelineLayout);
            pipelineLayout = nullptr;
        }
        if (materialLayout) {
            device.destroyDescriptorSetLayout(materialLayout);
            materialLayout = nullptr;
        }
        // Pipelines destroyed by unique_ptr
        opaquePipeline.reset();
        transparentPipeline.reset();
    }

    // =========================================================================
    // Main Render Entry Point
    // =========================================================================

    void BasicTechnique::render(
        vk::CommandBuffer cmd,
        const DrawContext& drawContext,
        const GPUSceneData& sceneData,
        DescriptorAllocatorGrowable& frameDescriptors
    ) {
        // Performance tracking
        auto& stats = services::RenderingStats::Instance();
        stats.drawcallCount = 0;
        stats.triangleCount = 0;
        auto start = std::chrono::system_clock::now();

        // Get render targets from the renderer
        Image& sceneImage = renderer->getSceneImage();
        Image& depthImage = renderer->getContext()->getDepthImage();

        // -----------------------------------------------------------------
        // Begin Dynamic Rendering
        // -----------------------------------------------------------------
        // Vulkan 1.3 dynamic rendering - no render pass objects needed!
        // Just specify what images we're rendering to.

        // Color attachment - where pixel colors are written
        vk::RenderingAttachmentInfo colorAttachment = graphics::attachmentInfo(
            sceneImage.imageView, nullptr, vk::ImageLayout::eColorAttachmentOptimal);

        // Depth attachment - for depth testing (closer objects hide farther ones)
        vk::RenderingAttachmentInfo depthAttachment = graphics::depthAttachmentInfo(
            depthImage.imageView, vk::ImageLayout::eDepthAttachmentOptimal);

        // Define the rendering area
        const auto imageExtent = sceneImage.imageExtent;
        const vk::Rect2D rect{ vk::Offset2D{0, 0}, {imageExtent.width, imageExtent.height} };
        const vk::RenderingInfo renderInfo = graphics::renderingInfo(rect, &colorAttachment, &depthAttachment);

        cmd.beginRendering(&renderInfo);

        // -----------------------------------------------------------------
        // Allocate Scene Data Descriptor Set
        // -----------------------------------------------------------------
        // Each frame needs a new descriptor set pointing to the scene data buffer.
        // The per-frame allocator handles this efficiently.
        vk::DescriptorSet globalDescriptor = frameDescriptors.allocate(renderer->getSceneDataDescriptorLayout());
        {
            DescriptorWriter writer;
            writer.writeBuffer(0, renderer->getSceneDataBuffer().buffer, sizeof(GPUSceneData), 0, vk::DescriptorType::eUniformBuffer);
            writer.updateSet(renderer->getContext()->getDevice(), globalDescriptor);
        }

        // Draw all geometry
        renderGeometry(cmd, drawContext, sceneData, globalDescriptor);

        cmd.endRendering();

        // Record performance stats
        auto end = std::chrono::system_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        stats.meshDrawTime = static_cast<float>(elapsed.count()) / 1000.f;
    }

    // =========================================================================
    // Geometry Rendering
    // =========================================================================

    void BasicTechnique::renderGeometry(
        vk::CommandBuffer cmd,
        const DrawContext& drawContext,
        const GPUSceneData& sceneData,
        vk::DescriptorSet globalDescriptor
    ) {
        auto& stats = services::RenderingStats::Instance();

        // Reset state cache for this frame
        lastPipeline = nullptr;
        lastMaterial = nullptr;
        lastIndexBuffer = nullptr;

        // -----------------------------------------------------------------
        // Frustum Culling and Sorting
        // -----------------------------------------------------------------
        // Build a list of visible opaque objects
        std::vector<u32> opaqueDraws;
        opaqueDraws.reserve(drawContext.opaqueSurfaces.size());

        for (uint32_t i = 0; i < drawContext.opaqueSurfaces.size(); i++) {
            if (isVisible(drawContext.opaqueSurfaces[i], sceneData.viewProj)) {
                opaqueDraws.push_back(i);
            }
        }

        // Sort by material, then by mesh
        // This minimizes expensive state changes (pipeline binds, descriptor binds)
        std::ranges::sort(opaqueDraws, [&](const auto& iA, const auto& iB) {
            const RenderObject& A = drawContext.opaqueSurfaces[iA];
            const RenderObject& B = drawContext.opaqueSurfaces[iB];
            if (A.material == B.material) {
                return A.indexBuffer < B.indexBuffer;  // Same material: sort by mesh
            }
            return A.material < B.material;  // Different materials: sort by material
        });

        // -----------------------------------------------------------------
        // Set Dynamic State
        // -----------------------------------------------------------------
        const auto imageExtent = renderer->getSceneImage().imageExtent;

        // Viewport: Maps [-1,1] to screen coordinates
        vk::Viewport viewport{};
        viewport.x = 0;
        viewport.y = 0;
        viewport.width = static_cast<float>(imageExtent.width);
        viewport.height = static_cast<float>(imageExtent.height);
        viewport.minDepth = 0.f;
        viewport.maxDepth = 1.f;
        cmd.setViewport(0, 1, &viewport);

        // Scissor: Clips to this rectangle (usually full screen)
        vk::Rect2D scissor{{0, 0}, {imageExtent.width, imageExtent.height}};
        cmd.setScissor(0, 1, &scissor);

        // -----------------------------------------------------------------
        // Draw Lambda
        // -----------------------------------------------------------------
        // This lambda draws a single object with optimal state management.
        auto draw = [&](const RenderObject& r) {
            // Only rebind if material changed
            if (r.material != lastMaterial) {
                lastMaterial = r.material;

                // Only rebind pipeline if it's different
                if (r.material->pipeline != lastPipeline) {
                    lastPipeline = r.material->pipeline;
                    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, r.material->pipeline->getPipeline());
                    // Bind scene data (set 0) when pipeline changes
                    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, r.material->pipeline->getLayout(), 0, 1, &globalDescriptor, 0, nullptr);
                }
                // Bind material data (set 1)
                cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, r.material->pipeline->getLayout(), 1, 1, &r.material->materialSet, 0, nullptr);
            }

            // Only rebind index buffer if it's different
            if (r.indexBuffer != lastIndexBuffer) {
                lastIndexBuffer = r.indexBuffer;
                cmd.bindIndexBuffer(r.indexBuffer, 0, vk::IndexType::eUint32);
            }

            // Push constants: per-object data (world matrix, vertex buffer address)
            GraphicsPushConstants pushConstants{};
            pushConstants.vertexBuffer = r.vertexBufferAddress;
            pushConstants.worldMatrix = r.transform;
            cmd.pushConstants(r.material->pipeline->getLayout(), vk::ShaderStageFlagBits::eVertex, 0, sizeof(GraphicsPushConstants), &pushConstants);

            // THE ACTUAL DRAW CALL
            // Draws r.indexCount indices starting at r.firstIndex
            cmd.drawIndexed(r.indexCount, 1, r.firstIndex, 0, 0);

            // Update stats
            stats.drawcallCount++;
            stats.triangleCount += static_cast<int>(r.indexCount / 3);
        };

        // -----------------------------------------------------------------
        // Draw Opaque Objects
        // -----------------------------------------------------------------
        // Draw sorted opaque objects (already sorted for minimal state changes)
        for (auto& r : opaqueDraws) {
            draw(drawContext.opaqueSurfaces[r]);
        }

        // -----------------------------------------------------------------
        // Draw Transparent Objects
        // -----------------------------------------------------------------
        // Transparent objects are drawn after opaque (order matters for blending)
        // They use the transparent pipeline (additive blending, no depth write)
        for (auto& r : drawContext.transparentSurfaces) {
            if (isVisible(r, sceneData.viewProj)) {
                draw(r);
            }
        }
    }

} // namespace graphics::techniques
