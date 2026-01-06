#include "Pipeline.h"
#include "../BasicServices/Log.h"
#include "../BasicServices/File.h"
#include "Types.h"
#include "Utils.hpp"

using services::File;
using services::Log;

namespace graphics {
    Pipeline::Pipeline(VulkanContext *context, vk::Pipeline pipeline, vk::PipelineLayout pipelineLayout)
    : context(context), graphicsPipeline(pipeline), pipelineLayout(pipelineLayout) {

        // Copy to be able to execute when object is out of scope
        vk::Device contextDevice = context->getDevice();
        vk::Pipeline pipelineCopy = pipeline;
        vk::PipelineLayout pipelineLayoutCopy = pipelineLayout;
    }

    Pipeline::~Pipeline() {
        context->getDevice().destroyPipelineLayout(pipelineLayout);
        context->getDevice().destroyPipeline(graphicsPipeline);
    }

    void Pipeline::bind(vk::CommandBuffer commandBuffer) const {
        commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, graphicsPipeline);
    }
}
