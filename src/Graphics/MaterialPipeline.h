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

        [[nodiscard]] vk::Pipeline getPipeline() const { return graphicsPipeline; }
        [[nodiscard]] vk::PipelineLayout getLayout() const { return pipelineLayout; }

        void setLayout(vk::PipelineLayout layout) { pipelineLayout = layout; }

        void bind(vk::CommandBuffer commandBuffer) const;

    private:
        VulkanContext* context;
        vk::Pipeline graphicsPipeline {nullptr};
        vk::PipelineLayout pipelineLayout {nullptr};  // Not owned, do not destroy
    };

} // namespace graphics
