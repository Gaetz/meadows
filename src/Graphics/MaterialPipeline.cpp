#include "MaterialPipeline.h"
#include "VulkanContext.h"
#include "../BasicServices/Log.h"
#include "../BasicServices/File.h"
#include "Types.h"
#include "Utils.hpp"

using services::File;
using services::Log;

namespace graphics {
    MaterialPipeline::MaterialPipeline(VulkanContext *context, vk::Pipeline pipeline, vk::PipelineLayout pipelineLayout)
    : context(context), graphicsPipeline(pipeline), pipelineLayout(pipelineLayout) {
    }

    MaterialPipeline::~MaterialPipeline() {
        // Note: pipelineLayout is not owned by MaterialPipeline, it's managed by the material system
        context->getDevice().destroyPipeline(graphicsPipeline);
    }

    void MaterialPipeline::bind(vk::CommandBuffer commandBuffer) const {
        commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, graphicsPipeline);
    }
}
