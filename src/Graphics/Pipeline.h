#pragma once

#include "Types.h"
#include "VulkanContext.h"

namespace graphics {


class Pipeline {
public:
    Pipeline(VulkanContext* context, vk::Pipeline pipeline, vk::PipelineLayout pipelineLayout);
    ~Pipeline();

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    void bind(vk::CommandBuffer commandBuffer) const;

private:
    VulkanContext* context;
    vk::Pipeline graphicsPipeline;
    vk::PipelineLayout pipelineLayout;
};

} // namespace graphics
