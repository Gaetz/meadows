#pragma once

#include "VulkanContext.h"
#include <vulkan/vulkan.hpp>
#include <string>
#include <vector>

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
    Pipeline(VulkanContext* context, const std::string& vertFilepath, const std::string& fragFilepath, const PipelineConfigInfo& configInfo);
    ~Pipeline();

    void bind(vk::CommandBuffer commandBuffer);

    static void defaultPipelineConfigInfo(PipelineConfigInfo& configInfo);

private:
    static std::vector<char> readFile(const std::string& filepath);
    void createGraphicsPipeline(const std::string& vertFilepath, const std::string& fragFilepath, const PipelineConfigInfo& configInfo);
    void createShaderModule(const std::vector<char>& code, vk::ShaderModule* shaderModule);

    VulkanContext* context;
    vk::Pipeline graphicsPipeline;
    vk::ShaderModule vertShaderModule;
    vk::ShaderModule fragShaderModule;
};
