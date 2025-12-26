#include "ComputeEffect.h"

namespace graphics {
    ComputeEffect::ComputeEffect(const char* nameP, VulkanContext* context, const str& compFilepath, const vk::PipelineLayout computePipelineLayout)
        : name { nameP },
        pipelineCompute(context, compFilepath, computePipelineLayout),
        data {}
    {

    }

    void ComputeEffect::bind(vk::CommandBuffer commandBuffer) {
        pipelineCompute.bind(commandBuffer);
    }

    vk::Pipeline ComputeEffect::getPipeline() const {
        return pipelineCompute.get();
    }

    vk::PipelineLayout ComputeEffect::getPipelineLayout() const {
        return pipelineCompute.getLayout();
    }
}
