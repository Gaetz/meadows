#pragma once
#include <vector>

#include "Pipeline.h"

namespace graphics {
    class PipelineBuilder {
    public:
        VulkanContext* context {nullptr};

        vk::ShaderModule vertexShaderModule {nullptr};
        vk::ShaderModule fragmentShaderModule {nullptr};

        vector<vk::PipelineShaderStageCreateInfo> shaderStages;
        vk::PipelineInputAssemblyStateCreateInfo inputAssembly;
        vk::PipelineRasterizationStateCreateInfo rasterizer;
        vk::PipelineColorBlendAttachmentState colorBlendAttachment;
        vk::PipelineMultisampleStateCreateInfo multisampling;
        vk::PipelineLayout pipelineLayout;
        vk::PipelineDepthStencilStateCreateInfo depthStencil;
        vk::PipelineRenderingCreateInfo renderInfo;
        vk::Format colorAttachmentFormat;

        PipelineBuilder(VulkanContext* context);
        PipelineBuilder(VulkanContext* context, const str& vertFilePath, const str& fragFilePath);

        void clear();

        [[nodiscard]] uptr<Pipeline> buildPipeline(vk::Device device) const;

        void setShaders(vk::ShaderModule vertexShader, vk::ShaderModule fragmentShader);

        void setInputTopology(vk::PrimitiveTopology topology);

        void setPolygonMode(vk::PolygonMode mode);

        void setCullMode(vk::CullModeFlags cullMode, vk::FrontFace frontFace);

        void setMultisamplingNone();

        void disableBlending();

        void setColorAttachmentFormat(vk::Format format);

        void setDepthFormat(vk::Format format);

        void disableDepthTest();

        void enableDepthTest(bool deepWriteEnable, vk::CompareOp compareOp);
    };
} // namespace graphics
