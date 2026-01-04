//
// Created by dsin on 09/12/2025.
//

#include "PipelineCompute.h"

#include "Utils.hpp"
#include "VulkanInit.hpp"
#include "BasicServices/File.h"

using services::File;

namespace graphics {
    PipelineCompute::PipelineCompute(VulkanContext *context, const str& compFilepath, const vk::PipelineLayout computePipelineLayout) {
        this->context = context;
        this->computePipelineLayout = computePipelineLayout;
        createComputePipeline(compFilepath);
    }

    void PipelineCompute::bind(vk::CommandBuffer commandBuffer) {
        commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, computePipeline);
    }

    void PipelineCompute::createComputePipeline(const str& compFilepath) {
        const auto compCode = File::readBinary(compFilepath);
        compShaderModule = createShaderModule(compCode, context->getDevice());

        const vk::PipelineShaderStageCreateInfo stageInfo = graphics::shaderStageCreateInfo(vk::ShaderStageFlagBits::eCompute, compShaderModule);

        vk::ComputePipelineCreateInfo computePipelineCreateInfo{};
        computePipelineCreateInfo.layout = computePipelineLayout;
        computePipelineCreateInfo.stage = stageInfo;

        computePipeline = context->getDevice().createComputePipeline(nullptr, computePipelineCreateInfo).value;

        context->getDevice().destroyShaderModule(compShaderModule);

        vk::Device contextDevice = context->getDevice();
        vk::Pipeline pipelineCopy = computePipeline;
        context->addToMainDeletionQueue([pipelineCopy, contextDevice]() {
                contextDevice.destroyPipeline(pipelineCopy);
            }
        , "computePipeline");
    }
}
