/**
 * @file ComputeEffect.cpp
 * @brief Implementation of the named compute effect wrapper.
 *
 * ComputeEffect provides a convenient way to manage compute shaders
 * with associated names and configurable push constant data.
 */

#include "ComputeEffect.h"

namespace graphics {

    // =========================================================================
    // Constructor
    // =========================================================================

    ComputeEffect::ComputeEffect(const char* nameP, VulkanContext* context, const str& compFilepath, const vk::PipelineLayout computePipelineLayout)
        : name { nameP },
        pipelineCompute(context, compFilepath, computePipelineLayout),  // Create the underlying pipeline
        data {}  // Initialize push constants to zero
    {
        // The actual pipeline creation happens in PipelineCompute's constructor
    }

    // =========================================================================
    // Pipeline Binding
    // =========================================================================

    void ComputeEffect::bind(vk::CommandBuffer commandBuffer) {
        // Delegate to the underlying pipeline
        pipelineCompute.bind(commandBuffer);
    }

    // =========================================================================
    // Accessors
    // =========================================================================

    vk::Pipeline ComputeEffect::getPipeline() const {
        return pipelineCompute.get();
    }

    vk::PipelineLayout ComputeEffect::getPipelineLayout() const {
        return pipelineCompute.getLayout();
    }
}
