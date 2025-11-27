#pragma once

#include "../Defines.h"
#include "VulkanContext.h"
#include <vulkan/vulkan.hpp>
#include <vector>

namespace graphics {

struct PipelineConfigInfo {
    vk::PipelineViewportStateCreateInfo viewportInfo;
    vk::PipelineInputAssemblyStateCreateInfo inputAssemblyInfo;
    vk::PipelineRasterizationStateCreateInfo rasterizationInfo;
    vk::PipelineMultisampleStateCreateInfo multisampleInfo;
    vk::PipelineColorBlendAttachmentState colorBlendAttachment;
    vk::PipelineColorBlendStateCreateInfo colorBlendInfo;
    vk::PipelineDepthStencilStateCreateInfo depthStencilInfo;
    std::vector<vk::DynamicState> dynamicStateEnables;
    vk::PipelineDynamicStateCreateInfo dynamicStateInfo;
    vk::PipelineLayout pipelineLayout = nullptr;
    vk::RenderPass renderPass = nullptr;
    uint32_t subpass = 0;
};

class Pipeline {
public:
    Pipeline(VulkanContext* context, const str& vertFilepath, const str& fragFilepath, const PipelineConfigInfo& configInfo);
    ~Pipeline();

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    void bind(vk::CommandBuffer commandBuffer);

    static void defaultPipelineConfigInfo(PipelineConfigInfo& configInfo);

private:
    void createGraphicsPipeline(const str& vertFilepath, const str& fragFilepath, const PipelineConfigInfo& configInfo);
    void createShaderModule(const std::vector<char>& code, vk::ShaderModule* shaderModule);

    VulkanContext* context;
    vk::Pipeline graphicsPipeline;
    vk::ShaderModule vertShaderModule;
    vk::ShaderModule fragShaderModule;
};

} // namespace graphics
