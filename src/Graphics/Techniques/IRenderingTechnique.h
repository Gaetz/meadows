/**
 * @file IRenderingTechnique.h
 * @brief Interface for pluggable rendering techniques.
 *
 * This file defines the base interface that all rendering techniques must implement,
 * enabling a flexible rendering architecture where different techniques can be swapped.
 */

#pragma once

#include <vulkan/vulkan.hpp>
#include "../Types.h"
#include "../RenderObject.h"

namespace graphics {
    class Renderer;
    class DescriptorAllocatorGrowable;
    struct FrameData;
}

namespace graphics::techniques {

    /**
     * @enum TechniqueType
     * @brief Identifies the type of rendering technique.
     *
     * Used for runtime identification and configuration of techniques.
     */
    enum class TechniqueType {
        Basic,          ///< Simple forward rendering without shadows
        ShadowMapping,  ///< Forward rendering with shadow mapping
        Deferred,       ///< Deferred rendering with G-Buffer
        // Add more technique types as needed
    };

    /**
     * @class IRenderingTechnique
     * @brief Interface for all rendering techniques.
     *
     * ## What is a Rendering Technique?
     * A rendering technique defines HOW the scene is rendered. Different techniques
     * offer different trade-offs between visual quality, performance, and features.
     *
     * ## Examples of techniques:
     * - **Basic**: Simple forward rendering, fast but limited lighting
     * - **Shadow Mapping**: Forward rendering with real-time shadows
     * - **Deferred**: G-Buffer based, supports many lights efficiently
     *
     * ## How techniques fit into the rendering pipeline:
     * The Renderer class orchestrates the overall frame:
     * 1. Waits for previous frame to complete
     * 2. Acquires swapchain image
     * 3. Calls technique->render() for scene rendering
     * 4. Applies post-processing (bloom, SSAO)
     * 5. Copies result to swapchain
     * 6. Renders ImGui
     * 7. Presents to screen
     *
     * The technique handles step 3 - the actual scene geometry rendering.
     *
     * ## Implementing a new technique:
     * @code
     * class MyTechnique : public IRenderingTechnique {
     * public:
     *     void init(Renderer* renderer) override {
     *         // Create pipelines, descriptor layouts, etc.
     *     }
     *     void cleanup(vk::Device device) override {
     *         // Destroy Vulkan resources
     *     }
     *     void render(vk::CommandBuffer cmd, const DrawContext& ctx,
     *                 const GPUSceneData& sceneData,
     *                 DescriptorAllocatorGrowable& frameDescriptors) override {
     *         // Record rendering commands
     *     }
     * };
     * @endcode
     */
    class IRenderingTechnique {
    public:
        virtual ~IRenderingTechnique() = default;

        /**
         * @brief Initializes the technique (creates pipelines, layouts, etc.).
         * @param renderer The renderer that owns this technique.
         *
         * Called once when the technique is first used.
         */
        virtual void init(Renderer* renderer) = 0;

        /**
         * @brief Cleans up all Vulkan resources.
         * @param device The Vulkan device.
         *
         * Called when the technique is no longer needed.
         */
        virtual void cleanup(vk::Device device) = 0;

        /**
         * @brief Renders the scene using this technique.
         * @param cmd The command buffer to record commands into.
         * @param drawContext Contains all objects to render (opaque and transparent).
         * @param sceneData Global scene data (matrices, lighting).
         * @param frameDescriptors Per-frame descriptor allocator.
         *
         * This is the main entry point for rendering. The technique should:
         * 1. Set up render pass(es)
         * 2. Bind pipeline(s)
         * 3. Draw all objects from drawContext
         * 4. Handle any technique-specific passes (shadows, G-Buffer, etc.)
         */
        virtual void render(
            vk::CommandBuffer cmd,
            const DrawContext& drawContext,
            const GPUSceneData& sceneData,
            DescriptorAllocatorGrowable& frameDescriptors
        ) = 0;

        /**
         * @brief Returns whether this technique requires a separate shadow pass.
         *
         * If true, the renderer may need to set up shadow resources.
         */
        virtual bool requiresShadowPass() const { return false; }

        /// Returns the technique type for identification
        virtual const TechniqueType getTechnique() const = 0;

        /// Returns a human-readable name for the technique
        virtual const str getName() const = 0;

    };

} // namespace graphics::techniques
