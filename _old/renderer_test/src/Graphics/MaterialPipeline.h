/**
 * @file MaterialPipeline.h
 * @brief Wrapper for a graphics pipeline used for material rendering.
 */

#pragma once

#include "Types.h"

namespace graphics {
    class VulkanContext;

    /**
     * @class MaterialPipeline
     * @brief Owns and manages a Vulkan graphics pipeline for rendering materials.
     *
     * ## What is a MaterialPipeline?
     * A MaterialPipeline wraps a Vulkan graphics pipeline (VkPipeline) and provides
     * RAII (Resource Acquisition Is Initialization) semantics for automatic cleanup.
     *
     * ## Pipeline vs Layout ownership:
     * - **Pipeline**: OWNED by this class, destroyed in destructor
     * - **Layout**: NOT OWNED, managed by the material system (e.g., GLTFMetallicRoughness)
     *
     * This separation exists because multiple pipelines often share the same layout
     * (e.g., opaque and transparent variants of the same material).
     *
     * ## Why separate from PipelineBuilder?
     * - PipelineBuilder: Creates pipelines with a fluent API
     * - MaterialPipeline: Owns and manages the created pipeline's lifetime
     *
     * ## Typical usage:
     * @code
     * // Created by PipelineBuilder
     * uptr<MaterialPipeline> pipeline = builder.buildPipeline(device);
     *
     * // In render loop
     * pipeline->bind(commandBuffer);
     * // Draw calls...
     *
     * // Pipeline automatically destroyed when unique_ptr goes out of scope
     * @endcode
     *
     * @note Copy is disabled to prevent accidental double-destruction of the pipeline.
     */
    class MaterialPipeline {
    public:
        /**
         * @brief Creates a MaterialPipeline wrapper.
         * @param context The Vulkan context (needed for cleanup).
         * @param pipeline The graphics pipeline to manage (ownership transferred).
         * @param pipelineLayout The pipeline layout (NOT owned, just referenced).
         */
        MaterialPipeline(VulkanContext* context, vk::Pipeline pipeline, vk::PipelineLayout pipelineLayout);

        /**
         * @brief Destructor - destroys the owned graphics pipeline.
         *
         * The pipeline layout is NOT destroyed here as it's not owned by this class.
         */
        ~MaterialPipeline();

        // Copy disabled - each MaterialPipeline owns its VkPipeline
        MaterialPipeline(const MaterialPipeline&) = delete;
        MaterialPipeline& operator=(const MaterialPipeline&) = delete;

        /// Returns the Vulkan pipeline handle
        [[nodiscard]] vk::Pipeline getPipeline() const { return graphicsPipeline; }

        /// Returns the pipeline layout (for binding descriptor sets and push constants)
        [[nodiscard]] vk::PipelineLayout getLayout() const { return pipelineLayout; }

        /// Sets the pipeline layout reference
        void setLayout(vk::PipelineLayout layout) { pipelineLayout = layout; }

        /**
         * @brief Binds this pipeline for graphics rendering.
         * @param commandBuffer The command buffer to record into.
         *
         * After binding, you can issue draw commands that will use this pipeline's
         * shaders and state configuration.
         */
        void bind(vk::CommandBuffer commandBuffer) const;

    private:
        VulkanContext* context;                        ///< Vulkan context for device access
        vk::Pipeline graphicsPipeline {nullptr};       ///< The owned graphics pipeline
        vk::PipelineLayout pipelineLayout {nullptr};   ///< Pipeline layout (NOT owned, do not destroy)
    };

} // namespace graphics
