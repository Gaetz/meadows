#include "Pipeline.h"
#include "../BasicServices/Log.h"
#include "../BasicServices/File.h"
#include "Types.h"
#include "Utils.hpp"

using services::File;
using services::Log;

namespace graphics {
    Pipeline::Pipeline(VulkanContext *context, const str &vertFilepath, const str &fragFilepath)
        : context(context) {
        createGraphicsPipeline(vertFilepath, fragFilepath);
    }

    Pipeline::~Pipeline() {
        context->getDevice().destroyShaderModule(vertShaderModule);
        context->getDevice().destroyShaderModule(fragShaderModule);
        context->getDevice().destroyPipeline(graphicsPipeline);
    }

    void Pipeline::bind(vk::CommandBuffer commandBuffer) {
        commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, graphicsPipeline);
    }

    void Pipeline::createGraphicsPipeline(const str &vertFilepath, const str &fragFilepath) {
        auto vertCode = File::readBinary(vertFilepath);
        auto fragCode = File::readBinary(fragFilepath);

        const vk::Device device = context->getDevice();
        vertShaderModule = graphics::createShaderModule(vertCode, device);
        fragShaderModule = graphics::createShaderModule(fragCode, device);
    }
} // namespace graphics
