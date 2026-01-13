/**
 * @file PipelineBuilder.cpp
 * @brief Implementation of the Graphics Pipeline builder.
 *
 * This file implements a builder pattern that simplifies the creation of
 * Vulkan graphics pipelines, which require extensive configuration.
 */

#include "PipelineBuilder.h"

#include "MaterialPipeline.h"
#include "VulkanContext.h"
#include "Utils.hpp"
#include "VulkanInit.hpp"
#include "BasicServices/File.h"
#include "BasicServices/Log.h"

namespace graphics {

    // =========================================================================
    // Constructors
    // =========================================================================

    PipelineBuilder::PipelineBuilder(VulkanContext* context) : context(context) {
        clear();
    }

    PipelineBuilder::PipelineBuilder(VulkanContext* context, const str &vertFilePath, const str &fragFilePath)
    : context(context) {
        clear();

        // Load and create shader modules from SPIR-V files
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

    // =========================================================================
    // Builder Reset
    // =========================================================================

    void PipelineBuilder::clear() {
        // Reset input assembly to triangle list (most common)
        inputAssembly = vk::PipelineInputAssemblyStateCreateInfo();
        inputAssembly.topology = vk::PrimitiveTopology::eTriangleList;
        inputAssembly.primitiveRestartEnable = false;

        // Reset rasterizer with default line width
        rasterizer = vk::PipelineRasterizationStateCreateInfo();
        rasterizer.lineWidth = 1.0f;

        // Clear other state
        colorBlendAttachment = vk::PipelineColorBlendAttachmentState{};
        multisampling = vk::PipelineMultisampleStateCreateInfo{};
        pipelineLayout = vk::PipelineLayout{};
        depthStencil = vk::PipelineDepthStencilStateCreateInfo{};
        renderInfo = vk::PipelineRenderingCreateInfo{};
        shaderStages.clear();
        depthOnlyMode = false;
        depthBiasEnable = false;
    }

    // =========================================================================
    // Pipeline Building
    // =========================================================================

    uptr<MaterialPipeline> PipelineBuilder::buildPipeline(const vk::Device device) const {
        // Apply fragment specialization constants if set
        std::vector<vk::PipelineShaderStageCreateInfo> finalShaderStages = shaderStages;
        if (fragmentSpecialization.has_value() && finalShaderStages.size() > 1) {
            // Fragment shader is typically the second stage (index 1)
            finalShaderStages[1].pSpecializationInfo = &fragmentSpecialization.value();
        }

        // Viewport state - we use dynamic viewport/scissor, so just specify count
        vk::PipelineViewportStateCreateInfo viewportState {};
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        // Color blending configuration
        vk::PipelineColorBlendStateCreateInfo colorBlending {};
        colorBlending.logicOpEnable = false;
        colorBlending.logicOp = vk::LogicOp::eCopy;

        if (depthOnlyMode) {
            // No color attachments for depth-only rendering (shadow maps)
            colorBlending.attachmentCount = 0;
            colorBlending.pAttachments = nullptr;
        } else {
            // Create blend state for each color attachment
            colorBlending.attachmentCount = static_cast<uint32_t>(renderInfo.colorAttachmentCount);
            static std::vector<vk::PipelineColorBlendAttachmentState> blendAttachments;
            blendAttachments.clear();
            for (uint32_t i = 0; i < renderInfo.colorAttachmentCount; ++i) {
                blendAttachments.push_back(colorBlendAttachment);
            }
            colorBlending.pAttachments = blendAttachments.data();
        }

        // Vertex input - empty because we use buffer device addresses
        // (vertices are fetched manually in the shader)
        vk::PipelineVertexInputStateCreateInfo vertexInputInfo {};

        // Copy rasterizer and apply depth bias setting
        vk::PipelineRasterizationStateCreateInfo rasterizerCopy = rasterizer;
        rasterizerCopy.depthBiasEnable = depthBiasEnable ? VK_TRUE : VK_FALSE;

        // Assemble the final pipeline create info
        vk::GraphicsPipelineCreateInfo pipelineInfo {};
        // Dynamic rendering - connect format info via pNext chain
        pipelineInfo.pNext = &renderInfo;

        pipelineInfo.stageCount = static_cast<uint32_t>(finalShaderStages.size());
        pipelineInfo.pStages = finalShaderStages.data();
        pipelineInfo.pVertexInputState = &vertexInputInfo;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizerCopy;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pColorBlendState = &colorBlending;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.layout = pipelineLayout;

        // Dynamic states - these can be changed per draw call without recreating the pipeline
        std::vector<vk::DynamicState> dynamicStates = {
            vk::DynamicState::eViewport,  // Viewport can change (window resize)
            vk::DynamicState::eScissor    // Scissor can change (UI clipping)
        };
        if (depthBiasEnable) {
            dynamicStates.push_back(vk::DynamicState::eDepthBias);  // For shadow mapping
        }

        vk::PipelineDynamicStateCreateInfo dynamicInfo {};
        dynamicInfo.pDynamicStates = dynamicStates.data();
        dynamicInfo.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
        pipelineInfo.pDynamicState = &dynamicInfo;

        // Create the pipeline!
        vk::Pipeline newPipeline {};
        if (device.createGraphicsPipelines(VK_NULL_HANDLE, 1, &pipelineInfo,nullptr, &newPipeline) != vk::Result::eSuccess) {
            services::Log::Error("Failed to create graphics pipeline");
        }

        return std::make_unique<MaterialPipeline>( context, newPipeline, pipelineLayout );
    }

    // =========================================================================
    // Shader Configuration
    // =========================================================================

    void PipelineBuilder::setShaders(const vk::ShaderModule vertexShader, const vk::ShaderModule fragmentShader) {
        shaderStages.clear();
        // Add vertex shader stage
        shaderStages.push_back(
            graphics::shaderStageCreateInfo(vk::ShaderStageFlagBits::eVertex, vertexShader));
        // Add fragment shader stage
        shaderStages.push_back(
            graphics::shaderStageCreateInfo(vk::ShaderStageFlagBits::eFragment, fragmentShader));
    }

    void PipelineBuilder::setVertexShaderOnly(vk::ShaderModule vertexShader) {
        shaderStages.clear();
        // Only add vertex shader - used for depth-only passes
        shaderStages.push_back(
            graphics::shaderStageCreateInfo(vk::ShaderStageFlagBits::eVertex, vertexShader));
    }

    void PipelineBuilder::setFragmentSpecialization(const vk::SpecializationInfo& specInfo) {
        fragmentSpecialization = specInfo;
    }

    void PipelineBuilder::destroyShaderModules(vk::Device device) {
        // Shader modules can be destroyed after pipeline creation
        if (vertexShaderModule) {
            device.destroyShaderModule(vertexShaderModule, nullptr);
            vertexShaderModule = nullptr;
        }
        if (fragmentShaderModule) {
            device.destroyShaderModule(fragmentShaderModule, nullptr);
            fragmentShaderModule = nullptr;
        }
    }

    // =========================================================================
    // Input Assembly
    // =========================================================================

    void PipelineBuilder::setInputTopology(const vk::PrimitiveTopology topology) {
        inputAssembly.topology = topology;
        inputAssembly.primitiveRestartEnable = false;
    }

    // =========================================================================
    // Rasterization
    // =========================================================================

    void PipelineBuilder::setPolygonMode(const vk::PolygonMode mode) {
        rasterizer.polygonMode = mode;
        rasterizer.lineWidth = 1.f;  // Required even for fill mode
    }

    void PipelineBuilder::setCullMode(vk::CullModeFlags cullMode, vk::FrontFace frontFace) {
        rasterizer.cullMode = cullMode;
        rasterizer.frontFace = frontFace;
    }

    // =========================================================================
    // Multisampling
    // =========================================================================

    void PipelineBuilder::setMultisamplingNone() {
        multisampling.sampleShadingEnable = false;
        multisampling.rasterizationSamples = vk::SampleCountFlagBits::e1;  // 1 sample = no MSAA
        multisampling.minSampleShading = 1.0f;
        multisampling.pSampleMask = nullptr;
        multisampling.alphaToCoverageEnable = false;
        multisampling.alphaToOneEnable = false;
    }

    // =========================================================================
    // Color Blending
    // =========================================================================

    void PipelineBuilder::disableBlending() {
        // Write all color channels
        colorBlendAttachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
        colorBlendAttachment.blendEnable = false;
    }

    void PipelineBuilder::enableBlendingAdditive() {
        // Additive: result = src * srcAlpha + dst * 1
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
        // Alpha blend: result = src * srcAlpha + dst * (1 - srcAlpha)
        colorBlendAttachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
        colorBlendAttachment.blendEnable = true;
        colorBlendAttachment.srcColorBlendFactor = vk::BlendFactor::eSrcAlpha;
        colorBlendAttachment.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
        colorBlendAttachment.colorBlendOp = vk::BlendOp::eAdd;
        colorBlendAttachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
        colorBlendAttachment.dstAlphaBlendFactor = vk::BlendFactor::eZero;
        colorBlendAttachment.alphaBlendOp = vk::BlendOp::eAdd;
    }

    // =========================================================================
    // Render Targets
    // =========================================================================

    void PipelineBuilder::setColorAttachmentFormat(const vk::Format format) {
        colorAttachmentFormats = { format };
        renderInfo.colorAttachmentCount = 1;
        renderInfo.pColorAttachmentFormats = colorAttachmentFormats.data();
    }

    void PipelineBuilder::setColorAttachmentFormats(std::span<vk::Format> formats) {
        colorAttachmentFormats.assign(formats.begin(), formats.end());
        renderInfo.colorAttachmentCount = static_cast<uint32_t>(colorAttachmentFormats.size());
        renderInfo.pColorAttachmentFormats = colorAttachmentFormats.data();
    }

    void PipelineBuilder::setDepthFormat(const vk::Format format) {
        renderInfo.depthAttachmentFormat = format;
    }

    // =========================================================================
    // Depth Testing
    // =========================================================================

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

    void PipelineBuilder::setDepthBiasEnable(bool enable) {
        depthBiasEnable = enable;
    }

    void PipelineBuilder::setDepthOnlyMode(bool enable) {
        depthOnlyMode = enable;
        if (enable) {
            // No color attachments for depth-only rendering
            renderInfo.colorAttachmentCount = 0;
            renderInfo.pColorAttachmentFormats = nullptr;
        }
    }
}
