#pragma once

#include "Types.h"
#include "VulkanContext.h"

namespace graphics {


class Pipeline {
public:
    Pipeline(VulkanContext* context, const str& vertFilepath, const str& fragFilepath);
    ~Pipeline();

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    void bind(vk::CommandBuffer commandBuffer);

private:
    void createGraphicsPipeline(const str& vertFilepath, const str& fragFilepath);
    void createShaderModule(const std::vector<char>& code, vk::ShaderModule* shaderModule);

    VulkanContext* context;
    vk::Pipeline graphicsPipeline;
    vk::ShaderModule vertShaderModule;
    vk::ShaderModule fragShaderModule;
};

} // namespace graphics
