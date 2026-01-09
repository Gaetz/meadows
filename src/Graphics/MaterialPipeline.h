#pragma once

#include "Types.h"
#include "VulkanContext.h"

namespace graphics {

    class MaterialPipeline {
    public:
        MaterialPipeline(VulkanContext* context, vk::Pipeline pipeline, vk::PipelineLayout pipelineLayout);
        ~MaterialPipeline();

        MaterialPipeline(const MaterialPipeline&) = delete;
        MaterialPipeline& operator=(const MaterialPipeline&) = delete;

        vk::Pipeline getPipeline() const { return graphicsPipeline; }
        vk::PipelineLayout getPipelineLayout() const { return pipelineLayout; }

        void bind(vk::CommandBuffer commandBuffer) const;

    private:
        VulkanContext* context;
        vk::Pipeline graphicsPipeline;
        vk::PipelineLayout pipelineLayout;
    };

} // namespace graphics
