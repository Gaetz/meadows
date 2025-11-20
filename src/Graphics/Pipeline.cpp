#include "Pipeline.h"
#include "Types.h"
#include <fstream>
#include <iostream>

Pipeline::Pipeline(VulkanContext* context, const std::string& vertFilepath, const std::string& fragFilepath, const PipelineConfigInfo& configInfo)
    : context(context) {
    createGraphicsPipeline(vertFilepath, fragFilepath, configInfo);
}

Pipeline::~Pipeline() {
    context->getDevice().destroyShaderModule(vertShaderModule);
    context->getDevice().destroyShaderModule(fragShaderModule);
    context->getDevice().destroyPipeline(graphicsPipeline);
}

void Pipeline::bind(vk::CommandBuffer commandBuffer) {
    commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, graphicsPipeline);
}

std::vector<char> Pipeline::readFile(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::ate | std::ios::binary);

    if (!file.is_open()) {
        throw std::runtime_error("failed to open file: " + filepath);
    }

    size_t fileSize = (size_t)file.tellg();
    std::vector<char> buffer(fileSize);

    file.seekg(0);
    file.read(buffer.data(), fileSize);
    file.close();

    return buffer;
}

void Pipeline::createGraphicsPipeline(const std::string& vertFilepath, const std::string& fragFilepath, const PipelineConfigInfo& configInfo) {
    auto vertCode = readFile(vertFilepath);
    auto fragCode = readFile(fragFilepath);

    createShaderModule(vertCode, &vertShaderModule);
    createShaderModule(fragCode, &fragShaderModule);

    vk::PipelineShaderStageCreateInfo shaderStages[] = {
        vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eVertex, vertShaderModule, "main"),
        vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eFragment, fragShaderModule, "main")
    };

    auto bindingDescription = Vertex::getBindingDescription();
    auto attributeDescriptions = Vertex::getAttributeDescriptions();

    vk::PipelineVertexInputStateCreateInfo vertexInputInfo(
        {},
        1, &bindingDescription,
        static_cast<uint32_t>(attributeDescriptions.size()), attributeDescriptions.data()
    );

    vk::GraphicsPipelineCreateInfo pipelineInfo(
        {},
        2, shaderStages,
        &vertexInputInfo,
        &configInfo.inputAssemblyInfo,
        nullptr, // Tessellation
        &configInfo.viewportInfo,
        &configInfo.rasterizationInfo,
        &configInfo.multisampleInfo,
        &configInfo.depthStencilInfo,
        &configInfo.colorBlendInfo,
        &configInfo.dynamicStateInfo,
        configInfo.pipelineLayout,
        configInfo.renderPass,
        configInfo.subpass,
        nullptr, // Base pipeline handle
        -1 // Base pipeline index
    );

    auto result = context->getDevice().createGraphicsPipeline(nullptr, pipelineInfo);
    if (result.result != vk::Result::eSuccess) {
        throw std::runtime_error("failed to create graphics pipeline!");
    }
    graphicsPipeline = result.value;
}

void Pipeline::createShaderModule(const std::vector<char>& code, vk::ShaderModule* shaderModule) {
    vk::ShaderModuleCreateInfo createInfo(
        {},
        code.size(),
        reinterpret_cast<const uint32_t*>(code.data())
    );

    *shaderModule = context->getDevice().createShaderModule(createInfo);
}

void Pipeline::defaultPipelineConfigInfo(PipelineConfigInfo& configInfo) {
    configInfo.inputAssemblyInfo = vk::PipelineInputAssemblyStateCreateInfo(
        {}, vk::PrimitiveTopology::eTriangleList, VK_FALSE
    );

    configInfo.viewportInfo = vk::PipelineViewportStateCreateInfo(
        {}, 1, nullptr, 1, nullptr
    );

    configInfo.rasterizationInfo = vk::PipelineRasterizationStateCreateInfo(
        {}, VK_FALSE, VK_FALSE, vk::PolygonMode::eFill, vk::CullModeFlagBits::eBack,
        vk::FrontFace::eCounterClockwise, VK_FALSE, 0.0f, 0.0f, 0.0f, 1.0f
    );

    configInfo.multisampleInfo = vk::PipelineMultisampleStateCreateInfo(
        {}, vk::SampleCountFlagBits::e1, VK_FALSE, 1.0f, nullptr, VK_FALSE, VK_FALSE
    );

    configInfo.colorBlendAttachment = vk::PipelineColorBlendAttachmentState(
        VK_FALSE,
        vk::BlendFactor::eOne, vk::BlendFactor::eZero, vk::BlendOp::eAdd,
        vk::BlendFactor::eOne, vk::BlendFactor::eZero, vk::BlendOp::eAdd,
        vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA
    );

    configInfo.colorBlendInfo = vk::PipelineColorBlendStateCreateInfo(
        {}, VK_FALSE, vk::LogicOp::eCopy, 1, &configInfo.colorBlendAttachment, {0.0f, 0.0f, 0.0f, 0.0f}
    );

    configInfo.depthStencilInfo = vk::PipelineDepthStencilStateCreateInfo(
        {}, VK_FALSE, VK_FALSE, vk::CompareOp::eLess, VK_FALSE, VK_FALSE, {}, {}, 0.0f, 1.0f
    );
    
    configInfo.dynamicStateEnables = { vk::DynamicState::eViewport, vk::DynamicState::eScissor };
    configInfo.dynamicStateInfo = vk::PipelineDynamicStateCreateInfo(
        {}, (uint32_t)configInfo.dynamicStateEnables.size(), configInfo.dynamicStateEnables.data()
    );
}
