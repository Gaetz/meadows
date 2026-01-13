/**
 * @file ShadowPipeline.h
 * @brief Pipelines for shadow mapping (depth pass, shadow rendering, and debug visualization).
 */

#pragma once

#include "Graphics/Types.h"
#include "Graphics/MaterialPipeline.h"

namespace graphics {
    class Renderer;
    class DescriptorAllocatorGrowable;
}

namespace graphics::pipelines {

    /**
     * @class ShadowPipeline
     * @brief Implements shadow mapping with multiple specialized pipelines.
     *
     * ## What is Shadow Mapping?
     * Shadow mapping is a two-pass technique for rendering shadows:
     * 1. **Depth Pass**: Render the scene from the light's perspective, storing only depth
     * 2. **Shadow Pass**: Render the scene normally, comparing each fragment's depth to the shadow map
     *
     * ## How does it work?
     * - In the depth pass, we create a "shadow map" - a depth texture from the light's viewpoint
     * - In the shadow pass, we transform each fragment to light space and compare depths
     * - If the fragment is farther than the shadow map value, it's in shadow
     *
     * ## The three pipelines:
     *
     * ### 1. Depth Pipeline (depthPipeline)
     * - Renders geometry from light's perspective
     * - Only outputs depth (no color attachments)
     * - Uses depth bias to prevent "shadow acne" (self-shadowing artifacts)
     * - Very fast - no fragment shader needed
     *
     * ### 2. Shadow Mesh Pipeline (shadowMeshPipeline)
     * - Renders the final scene with shadows applied
     * - Samples the shadow map to determine shadowed areas
     * - Uses the same material layout as GLTFMetallicRoughness
     *
     * ### 3. Debug Pipeline (debugPipeline)
     * - Visualizes the shadow map as a quad on screen
     * - Useful for debugging shadow issues
     *
     * ## Common shadow mapping issues:
     * - **Shadow Acne**: Self-shadowing artifacts (solved with depth bias)
     * - **Peter Panning**: Objects floating above ground (too much depth bias)
     * - **Aliasing**: Jagged shadow edges (use larger shadow maps or PCF filtering)
     */
    class ShadowPipeline {
    public:
        // =====================================================================
        // Pipelines
        // =====================================================================

        /// Depth-only pipeline for rendering the shadow map from light's perspective
        uptr<MaterialPipeline> depthPipeline { nullptr };

        /// Main rendering pipeline that applies shadows to the scene
        uptr<MaterialPipeline> shadowMeshPipeline { nullptr };

        /// Debug pipeline to visualize the shadow map on screen
        uptr<MaterialPipeline> debugPipeline { nullptr };

        // =====================================================================
        // Pipeline Layouts
        // =====================================================================

        /// Layout for depth pass (only needs scene data for light matrices)
        vk::PipelineLayout depthPipelineLayout { nullptr };

        /// Layout for shadow mesh pass (scene data + shadow map + materials)
        vk::PipelineLayout shadowMeshPipelineLayout { nullptr };

        /// Layout for debug visualization
        vk::PipelineLayout debugPipelineLayout { nullptr };

        /// Material descriptor layout (same structure as GLTFMetallicRoughness)
        vk::DescriptorSetLayout materialLayout { nullptr };

        /**
         * @brief Creates all three shadow pipelines.
         * @param renderer The renderer (provides context, formats, and descriptor layouts).
         */
        void buildPipelines(const Renderer* renderer);

        /**
         * @brief Destroys all Vulkan resources.
         * @param device The Vulkan device.
         */
        void clear(vk::Device device);

    private:
        /**
         * @brief Creates the depth-only pipeline for shadow map generation.
         *
         * Key characteristics:
         * - Vertex shader only (no fragment shader)
         * - No color attachments (depth only)
         * - Depth bias enabled to prevent shadow acne
         * - No face culling (to capture all shadow casters)
         */
        void buildDepthPipeline(const Renderer* renderer, vk::Device device);

        /**
         * @brief Creates the main pipeline that renders with shadows.
         *
         * Key characteristics:
         * - Uses shadow-aware shaders (meshShadow.vert/frag)
         * - Accesses shadow map via scene descriptor set
         * - Compares fragment depth to shadow map for shadow determination
         */
        void buildShadowMeshPipeline(const Renderer* renderer, vk::Device device);

        /**
         * @brief Creates the debug visualization pipeline.
         *
         * Renders the shadow map as a 2D quad for debugging purposes.
         */
        void buildDebugPipeline(const Renderer* renderer, vk::Device device);
    };

} // namespace graphics::pipelines
