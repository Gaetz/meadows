/**
 * @file BasicTechnique.h
 * @brief Simple forward rendering technique without shadows.
 */

#pragma once

#include "IRenderingTechnique.h"
#include "../MaterialPipeline.h"
#include "../DescriptorAllocatorGrowable.h"

namespace graphics::techniques {

    /**
     * @class BasicTechnique
     * @brief Implements simple forward rendering without shadows.
     *
     * ## What is Forward Rendering?
     * Forward rendering is the traditional approach where each object is fully
     * shaded in a single pass. For each pixel, lighting is computed immediately.
     *
     * ## Forward vs Deferred Rendering:
     * - **Forward**: Simple, good for few lights, supports transparency easily
     * - **Deferred**: Complex, supports many lights, transparency is tricky
     *
     * ## How BasicTechnique works:
     * 1. **Setup**: Create pipelines for opaque and transparent objects
     * 2. **Each frame**:
     *    - Begin rendering to the scene image
     *    - Draw all opaque objects (sorted by material to minimize state changes)
     *    - Draw all transparent objects (no depth write)
     *    - End rendering
     *
     * ## Optimizations implemented:
     * - **Frustum culling**: Skip objects outside the camera view
     * - **State sorting**: Sort by material to reduce pipeline/descriptor changes
     * - **Caching**: Track last pipeline/material/index buffer to avoid redundant binds
     *
     * ## Rendering steps in detail:
     * 1. **Cull objects**: Test bounding boxes against view frustum
     * 2. **Sort opaque objects**: By material, then by mesh
     * 3. **Begin render pass**: Attach color and depth images
     * 4. **Set viewport/scissor**: Define rendering area
     * 5. **For each object**:
     *    - Bind pipeline (if changed)
     *    - Bind descriptor sets (scene data, material)
     *    - Bind index buffer (if changed)
     *    - Push constants (world matrix, vertex buffer address)
     *    - Draw indexed
     * 6. **End render pass**
     */
    class BasicTechnique : public IRenderingTechnique {
    public:
        /**
         * @brief Initializes pipelines and descriptor layouts.
         *
         * Creates:
         * - Material descriptor layout (uniform buffer + 2 textures)
         * - Pipeline layout (scene data set + material set + push constants)
         * - Opaque pipeline (depth write enabled, no blending)
         * - Transparent pipeline (no depth write, additive blending)
         */
        void init(Renderer* renderer) override;

        /// Destroys all Vulkan resources
        void cleanup(vk::Device device) override;

        /**
         * @brief Renders the scene using forward rendering.
         *
         * Main entry point called by the Renderer each frame.
         * Handles the complete rendering pass from setup to draw calls.
         */
        void render(
            vk::CommandBuffer cmd,
            const DrawContext& drawContext,
            const GPUSceneData& sceneData,
            DescriptorAllocatorGrowable& frameDescriptors
        ) override;

        /// This technique doesn't use shadows
        bool requiresShadowPass() const override { return false; }

        const TechniqueType getTechnique() const override { return TechniqueType::Basic; }
        const str getName() const override { return std::move("Basic"); }

        // Accessors for external use (e.g., loading materials)
        vk::DescriptorSetLayout getMaterialLayout() const { return materialLayout; }
        vk::PipelineLayout getPipelineLayout() const { return pipelineLayout; }
        MaterialPipeline* getOpaquePipeline() const { return opaquePipeline.get(); }
        MaterialPipeline* getTransparentPipeline() const { return transparentPipeline.get(); }

    private:
        /**
         * @brief Internal method that performs the actual geometry rendering.
         *
         * This is where the actual draw calls happen:
         * - Sets up viewport and scissor
         * - Iterates through sorted opaque objects
         * - Iterates through visible transparent objects
         * - Issues draw calls with proper state management
         */
        void renderGeometry(
            vk::CommandBuffer cmd,
            const DrawContext& drawContext,
            const GPUSceneData& sceneData,
            vk::DescriptorSet globalDescriptor
        );

        Renderer* renderer { nullptr };  ///< Parent renderer reference

        // =====================================================================
        // Pipelines
        // =====================================================================
        uptr<MaterialPipeline> opaquePipeline;       ///< For solid objects (depth write, no blend)
        uptr<MaterialPipeline> transparentPipeline;  ///< For transparent objects (no depth write, blend)

        // =====================================================================
        // Descriptor Layouts
        // =====================================================================
        vk::DescriptorSetLayout materialLayout { nullptr };  ///< Material data (UBO + textures)
        vk::PipelineLayout pipelineLayout { nullptr };       ///< Scene + Material + Push constants

        // =====================================================================
        // State Caching (for optimization)
        // =====================================================================
        MaterialPipeline* lastPipeline { nullptr };   ///< Avoid redundant pipeline binds
        MaterialInstance* lastMaterial { nullptr };   ///< Avoid redundant material binds
        vk::Buffer lastIndexBuffer { nullptr };       ///< Avoid redundant index buffer binds
    };

} // namespace graphics::techniques
