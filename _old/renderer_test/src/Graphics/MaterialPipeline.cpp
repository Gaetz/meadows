/**
 * @file MaterialPipeline.cpp
 * @brief Implementation of the graphics pipeline wrapper for materials.
 *
 * MaterialPipeline provides RAII semantics for Vulkan graphics pipelines,
 * ensuring proper cleanup when the pipeline is no longer needed.
 */

#include "MaterialPipeline.h"
#include "VulkanContext.h"
#include "../BasicServices/Log.h"
#include "../BasicServices/File.h"
#include "Types.h"
#include "Utils.hpp"

using services::File;
using services::Log;

namespace graphics {

    // =========================================================================
    // Constructor
    // =========================================================================

    MaterialPipeline::MaterialPipeline(VulkanContext *context, vk::Pipeline pipeline, vk::PipelineLayout pipelineLayout)
    : context(context), graphicsPipeline(pipeline), pipelineLayout(pipelineLayout) {
        // Pipeline is now owned by this object
        // Layout is just a reference - it's managed elsewhere
    }

    // =========================================================================
    // Destructor
    // =========================================================================

    MaterialPipeline::~MaterialPipeline() {
        // Only destroy the pipeline - we own it
        // The pipelineLayout is NOT destroyed because:
        // 1. It's often shared between multiple pipelines (opaque/transparent)
        // 2. It's managed by the material system (e.g., GLTFMetallicRoughness)
        context->getDevice().destroyPipeline(graphicsPipeline);
    }

    // =========================================================================
    // Pipeline Binding
    // =========================================================================

    void MaterialPipeline::bind(vk::CommandBuffer commandBuffer) const {
        // Bind as a graphics pipeline (not compute)
        // After this call, subsequent draw commands will use this pipeline's configuration
        commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, graphicsPipeline);
    }
}
