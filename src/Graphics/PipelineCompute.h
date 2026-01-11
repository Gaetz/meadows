#pragma once
#include "Defines.h"
#include <vulkan/vulkan.hpp>

namespace graphics {
    class VulkanContext;

    class PipelineCompute {
    public:
        PipelineCompute(VulkanContext* context, const str& compFilepath, vk::PipelineLayout computePipelineLayout);
        ~PipelineCompute() = default;

        void bind(vk::CommandBuffer commandBuffer);
        [[nodiscard]] vk::Pipeline get() const { return computePipeline; }
        [[nodiscard]] vk::PipelineLayout getLayout() const { return computePipelineLayout; }

    private:
        void createComputePipeline(const str& compFilepath);

        VulkanContext* context;
        vk::PipelineLayout computePipelineLayout;
        vk::Pipeline computePipeline;
        vk::ShaderModule compShaderModule;
    };
}


