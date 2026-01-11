//
// Created by dsin on 04/01/2026.
//

#include "PipelineBuilder.h"

#include "Utils.hpp"
#include "VulkanInit.hpp"
#include "BasicServices/File.h"
#include "BasicServices/Log.h"

namespace graphics {
    PipelineBuilder::PipelineBuilder(VulkanContext* context) : context(context) {
        clear();
    }

    PipelineBuilder::PipelineBuilder(VulkanContext* context, const str &vertFilePath, const str &fragFilePath)
    : context(context) {
        clear();
        const vk::Device device = context->getDevice();
        vertexShaderModule = graphics::createShaderModule(
            services::File::readBinary(vertFilePath),
            device
        );
        fragmentShaderModule = graphics::createShaderModule(
            services::File::readBinary(fragFilePath),
            device
        );
        setShaders(vertexShaderModule, fragmentShaderModule);
    }

    void PipelineBuilder::clear() {
        inputAssembly = vk::PipelineInputAssemblyStateCreateInfo();
        rasterizer = vk::PipelineRasterizationStateCreateInfo();
        colorBlendAttachment = vk::PipelineColorBlendAttachmentState{};
        multisampling = vk::PipelineMultisampleStateCreateInfo{};
        pipelineLayout = vk::PipelineLayout{};
        depthStencil = vk::PipelineDepthStencilStateCreateInfo{};
        renderInfo = vk::PipelineRenderingCreateInfo{};
        shaderStages.clear();
    }

    uptr<MaterialPipeline> PipelineBuilder::buildPipeline(const vk::Device device) const {
        // Make viewport state from our stored viewport and scissor.
        // at the moment we won't support multiple viewports or scissors
        vk::PipelineViewportStateCreateInfo viewportState {};
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        // Setup dummy color blending. We aren't using transparent objects yet
        // the blending is just "no blend", but we do write to the color attachment
        vk::PipelineColorBlendStateCreateInfo colorBlending {};
        colorBlending.logicOpEnable = false;
        colorBlending.logicOp = vk::LogicOp::eCopy;
        colorBlending.attachmentCount = 1;
        colorBlending.pAttachments = &colorBlendAttachment;

        // Completely clear VertexInputStateCreateInfo, as we have no need for it
        vk::PipelineVertexInputStateCreateInfo vertexInputInfo {};

        // Build the actual pipeline
        vk::GraphicsPipelineCreateInfo pipelineInfo {};
        // Connect the renderInfo to the pNext extension mechanism
        pipelineInfo.pNext = &renderInfo;

        pipelineInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
        pipelineInfo.pStages = shaderStages.data();
        pipelineInfo.pVertexInputState = &vertexInputInfo;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pColorBlendState = &colorBlending;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.layout = pipelineLayout;

        constexpr vk::DynamicState state[] = { vk::DynamicState::eViewport, vk::DynamicState::eScissor };
        vk::PipelineDynamicStateCreateInfo dynamicInfo {};
        dynamicInfo.pDynamicStates = &state[0];
        dynamicInfo.dynamicStateCount = 2;
        pipelineInfo.pDynamicState = &dynamicInfo;

        vk::Pipeline newPipeline {};
        if (device.createGraphicsPipelines(VK_NULL_HANDLE, 1, &pipelineInfo,nullptr, &newPipeline) != vk::Result::eSuccess) {
            services::Log::Error("Failed to create graphics pipeline");
        }

        return std::make_unique<MaterialPipeline>( context, newPipeline, pipelineLayout );
    }

    void PipelineBuilder::setShaders(const vk::ShaderModule vertexShader, const vk::ShaderModule fragmentShader) {
        shaderStages.clear();
        shaderStages.push_back(
            graphics::shaderStageCreateInfo(vk::ShaderStageFlagBits::eVertex, vertexShader));
        shaderStages.push_back(
            graphics::shaderStageCreateInfo(vk::ShaderStageFlagBits::eFragment, fragmentShader));
    }

    void PipelineBuilder::setInputTopology(const vk::PrimitiveTopology topology) {
        inputAssembly.topology = topology;
        inputAssembly.primitiveRestartEnable = false;
    }

    void PipelineBuilder::setPolygonMode(const vk::PolygonMode mode) {
        rasterizer.polygonMode = mode;
        rasterizer.lineWidth = 1.f;
    }

    void PipelineBuilder::setCullMode(vk::CullModeFlags cullMode, vk::FrontFace frontFace) {
        rasterizer.cullMode = cullMode;
        rasterizer.frontFace = frontFace;
    }

    void PipelineBuilder::setMultisamplingNone() {
        multisampling.sampleShadingEnable = false;
        // Multisampling defaulted to no multisampling (1 sample per pixel)
        multisampling.rasterizationSamples = vk::SampleCountFlagBits::e1;
        multisampling.minSampleShading = 1.0f;
        multisampling.pSampleMask = nullptr;
        // No alpha to coverage either
        multisampling.alphaToCoverageEnable = false;
        multisampling.alphaToOneEnable = false;
    }

    void PipelineBuilder::disableBlending() {
        // Default write mask
        colorBlendAttachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
        // No blending
        colorBlendAttachment.blendEnable = false;
    }

    void PipelineBuilder::enableBlendingAdditive() {
        colorBlendAttachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
        colorBlendAttachment.blendEnable = true;
        colorBlendAttachment.srcColorBlendFactor = vk::BlendFactor::eSrcAlpha;
        colorBlendAttachment.dstColorBlendFactor = vk::BlendFactor::eOne;
        colorBlendAttachment.colorBlendOp = vk::BlendOp::eAdd;
        colorBlendAttachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
        colorBlendAttachment.dstAlphaBlendFactor = vk::BlendFactor::eZero;
        colorBlendAttachment.alphaBlendOp = vk::BlendOp::eAdd;
    }

    void PipelineBuilder::enableBlendingAlphaBlend() {
        colorBlendAttachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
        colorBlendAttachment.blendEnable = true;
        colorBlendAttachment.srcColorBlendFactor = vk::BlendFactor::eSrcAlpha;
        colorBlendAttachment.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
        colorBlendAttachment.colorBlendOp = vk::BlendOp::eAdd;
        colorBlendAttachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
        colorBlendAttachment.dstAlphaBlendFactor = vk::BlendFactor::eZero;
        colorBlendAttachment.alphaBlendOp = vk::BlendOp::eAdd;
    }

    void PipelineBuilder::setColorAttachmentFormat(const vk::Format format) {
        colorAttachmentFormat = format;
        // connect the format to the renderInfo  structure
        renderInfo.colorAttachmentCount = 1;
        renderInfo.pColorAttachmentFormats = &colorAttachmentFormat;
    }

    void PipelineBuilder::setDepthFormat(const vk::Format format) {
        renderInfo.depthAttachmentFormat = format;
    }

    void PipelineBuilder::disableDepthTest() {
        depthStencil.depthTestEnable = false;
        depthStencil.depthWriteEnable = false;
        depthStencil.depthCompareOp = vk::CompareOp::eNever;
        depthStencil.depthBoundsTestEnable = false;
        depthStencil.stencilTestEnable = false;
        depthStencil.front = vk::StencilOp::eZero;
        depthStencil.back = vk::StencilOp::eZero;
        depthStencil.minDepthBounds = 0.f;
        depthStencil.maxDepthBounds = 1.f;
    }

    void PipelineBuilder::enableDepthTest(bool deepWriteEnable, vk::CompareOp compareOp) {
        depthStencil.depthTestEnable = true;
        depthStencil.depthWriteEnable = deepWriteEnable;
        depthStencil.depthCompareOp = compareOp;
        depthStencil.depthBoundsTestEnable = false;
        depthStencil.stencilTestEnable = false;
        depthStencil.front = vk::StencilOp::eZero;
        depthStencil.back = vk::StencilOp::eZero;
        depthStencil.minDepthBounds = 0.f;
        depthStencil.maxDepthBounds = 1.f;
    }

    void PipelineBuilder::destroyShaderModules(vk::Device device) {
        if (vertexShaderModule) {
            device.destroyShaderModule(vertexShaderModule, nullptr);
            vertexShaderModule = nullptr;
        }
        if (fragmentShaderModule) {
            device.destroyShaderModule(fragmentShaderModule, nullptr);
            fragmentShaderModule = nullptr;
        }
    }
}
