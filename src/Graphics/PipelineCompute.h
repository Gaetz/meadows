#pragma once
#include "Defines.h"
#include <vulkan/vulkan.hpp>

namespace graphics {
    class VulkanContext;

    /**
     * @class PipelineCompute
     * @brief Represents a Vulkan Compute Pipeline.
     *
     * ## What is a Compute Pipeline?
     * Unlike graphics pipelines (which process vertices and fragments to render images),
     * compute pipelines run general-purpose GPU programs called "compute shaders".
     * They don't render anything directly but can process data in parallel on the GPU.
     *
     * ## Common uses for Compute Shaders:
     * - Image processing (blur, tone mapping, post-effects)
     * - Physics simulations (particles, cloth)
     * - Procedural generation (noise, terrain)
     * - GPU-driven rendering (culling, LOD selection)
     *
     * ## How Compute Shaders differ from Graphics Shaders:
     * - No vertex/fragment stages - just one compute stage
     * - Work in "work groups" (e.g., 16x16 threads processing pixels)
     * - Access resources via descriptor sets (same as graphics)
     * - Dispatched with vkCmdDispatch() instead of vkCmdDraw()
     *
     * ## Typical usage:
     * @code
     * // Create pipeline
     * PipelineCompute blurPipeline(context, "shaders/blur.comp.spv", computeLayout);
     *
     * // In render loop
     * blurPipeline.bind(commandBuffer);
     * commandBuffer.bindDescriptorSets(...);
     * commandBuffer.dispatch(imageWidth / 16, imageHeight / 16, 1);
     * @endcode
     */
    class PipelineCompute {
    public:
        /**
         * @brief Creates a compute pipeline from a compiled shader file.
         * @param context The Vulkan context (provides device access).
         * @param compFilepath Path to the compiled compute shader (.spv file).
         * @param computePipelineLayout The pipeline layout describing descriptor sets and push constants.
         *
         * The pipeline is automatically registered for cleanup when the context is destroyed.
         */
        PipelineCompute(VulkanContext* context, const str& compFilepath, vk::PipelineLayout computePipelineLayout);

        ~PipelineCompute() = default;

        /**
         * @brief Binds this compute pipeline for use in a command buffer.
         * @param commandBuffer The command buffer to record the bind command into.
         *
         * After binding, you can dispatch compute work with commandBuffer.dispatch().
         */
        void bind(vk::CommandBuffer commandBuffer);

        /// Returns the raw Vulkan pipeline handle
        [[nodiscard]] vk::Pipeline get() const { return computePipeline; }

        /// Returns the pipeline layout (describes descriptor sets and push constants)
        [[nodiscard]] vk::PipelineLayout getLayout() const { return computePipelineLayout; }

    private:
        /**
         * @brief Internal method to create the compute pipeline.
         * @param compFilepath Path to the compiled compute shader.
         *
         * This loads the shader, creates a shader module, builds the pipeline,
         * then destroys the shader module (it's no longer needed after pipeline creation).
         */
        void createComputePipeline(const str& compFilepath);

        VulkanContext* context;                      ///< Vulkan context for device access
        vk::PipelineLayout computePipelineLayout;    ///< Layout defining resources accessible to the shader
        vk::Pipeline computePipeline;                ///< The actual compute pipeline
        vk::ShaderModule compShaderModule;           ///< Temporary shader module (destroyed after pipeline creation)
    };
}


