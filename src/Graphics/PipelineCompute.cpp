//
// Created by dsin on 09/12/2025.
//

#include "PipelineCompute.h"

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
        auto compCode = File::readBinary(compFilepath);
        createShaderModule(compCode);

        vk::PipelineShaderStageCreateInfo stageInfo {};
        stageInfo.stage = vk::ShaderStageFlagBits::eCompute;
        stageInfo.module = compShaderModule;
        stageInfo.pName = "main";

        vk::ComputePipelineCreateInfo computePipelineCreateInfo{};
        computePipelineCreateInfo.layout = computePipelineLayout;
        computePipelineCreateInfo.stage = stageInfo;

        computePipeline = context->getDevice().createComputePipeline(nullptr, computePipelineCreateInfo).value;

        context->getDevice().destroyShaderModule(compShaderModule);
        context->addToMainDeletionQueue([&]() {
                context->getDevice().destroyPipeline(computePipeline);
            }
        );
    }

    void PipelineCompute::createShaderModule(const std::vector<char> &code) {
        vk::ShaderModuleCreateInfo createInfo(
            {},
            code.size(),
            reinterpret_cast<const uint32_t*>(code.data())
        );

        compShaderModule = context->getDevice().createShaderModule(createInfo);
    }
}
