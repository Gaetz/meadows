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
        vk::PipelineLayout getLayout() const { return pipelineLayout; }

        void setLayout(const vk::PipelineLayout layout) { pipelineLayout = layout; }

        void bind(vk::CommandBuffer commandBuffer) const;

    private:
        VulkanContext* context;
        vk::Pipeline graphicsPipeline;
        vk::PipelineLayout pipelineLayout;
    };

} // namespace graphics
