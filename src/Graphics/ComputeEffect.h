#pragma once
#include "PipelineCompute.h"
#include "Types.h"

namespace graphics {
    class ComputeEffect {
    private:
        PipelineCompute pipelineCompute;

    public:
        ComputePushConstants data;
        const char* name;

        ComputeEffect(const char* name, VulkanContext* context, const str& compFilepath, vk::PipelineLayout computePipelineLayout);
        void bind(vk::CommandBuffer commandBuffer);
        [[nodiscard]] vk::Pipeline getPipeline() const;
        [[nodiscard]] vk::PipelineLayout getPipelineLayout() const;
    };
}
