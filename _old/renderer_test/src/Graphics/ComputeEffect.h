/**
 * @file ComputeEffect.h
 * @brief Named compute effect with associated push constants.
 */

#pragma once
#include "PipelineCompute.h"
#include "Types.h"

namespace graphics {

    /**
     * @class ComputeEffect
     * @brief A named compute shader effect with configurable push constant data.
     *
     * ## What is a Compute Effect?
     * A ComputeEffect wraps a compute pipeline with a name and push constant data,
     * making it easier to manage multiple compute shaders in an application.
     *
     * ## When to use this?
     * Use ComputeEffect when you have multiple compute shaders that need to be:
     * - Identified by name (for debugging, profiling, or UI selection)
     * - Configured with different push constant values
     * - Easily swapped at runtime
     *
     * ## Example use cases:
     * - Multiple post-processing effects (blur, sharpen, edge detection)
     * - Different procedural generation algorithms
     * - Various image filters that can be toggled
     *
     * ## Relationship to PipelineCompute:
     * - PipelineCompute: Low-level wrapper around VkPipeline
     * - ComputeEffect: Higher-level abstraction with name and data
     *
     * ## Typical usage:
     * @code
     * ComputeEffect blurEffect("Gaussian Blur", context, "shaders/blur.comp.spv", layout);
     * blurEffect.data.data1 = glm::vec4(blurRadius, 0, 0, 0);
     *
     * blurEffect.bind(commandBuffer);
     * commandBuffer.pushConstants(blurEffect.getPipelineLayout(), ...);
     * commandBuffer.dispatch(width / 16, height / 16, 1);
     * @endcode
     */
    class ComputeEffect {
    private:
        PipelineCompute pipelineCompute;  ///< The underlying compute pipeline

    public:
        /**
         * @brief Push constant data to send to the shader.
         *
         * This data is pushed to the GPU each frame and can be modified
         * between dispatches to configure the effect's behavior.
         */
        ComputePushConstants data;

        /// Human-readable name for this effect (useful for debugging and UI)
        const char* name;

        /**
         * @brief Creates a new compute effect.
         * @param name Human-readable name for the effect.
         * @param context The Vulkan context.
         * @param compFilepath Path to the compiled compute shader (.spv).
         * @param computePipelineLayout The pipeline layout for this effect.
         */
        ComputeEffect(const char* name, VulkanContext* context, const str& compFilepath, vk::PipelineLayout computePipelineLayout);

        /**
         * @brief Binds this effect's pipeline for use.
         * @param commandBuffer The command buffer to record into.
         *
         * After binding, push constants and dispatch the compute work.
         */
        void bind(vk::CommandBuffer commandBuffer);

        /// Returns the underlying Vulkan pipeline
        [[nodiscard]] vk::Pipeline getPipeline() const;

        /// Returns the pipeline layout (needed for push constants)
        [[nodiscard]] vk::PipelineLayout getPipelineLayout() const;
    };
}
