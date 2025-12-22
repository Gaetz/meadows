#pragma once
#include "PipelineCompute.h"

namespace graphics {
    class ComputeEffect {
    private:
        PipelineCompute pipelineCompute;
        ComputePushConstants data;
        vk::PipelineLayout pipelineLayout;
        const char* name;

    public:
        ComputeEffect(VulkanContext* context, const str& compFilepath, const vk::PipelineLayout computePipelineLayout)
            : pipelineCompute(context, compFilepath, computePipelineLayout) {
        }

        void bind(vk::CommandBuffer commandBuffer) {
            pipelineCompute.bind(commandBuffer);
        }

        vk::Pipeline getPipeline() const {
            return pipelineCompute.get();
        }
    };
}
