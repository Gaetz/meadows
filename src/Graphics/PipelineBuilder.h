#pragma once
#include <vector>

#include "Types.h"

namespace graphics {
    class VulkanContext;
    class MaterialPipeline;

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
        std::vector<vk::Format> colorAttachmentFormats;

        // Depth-only pipeline support
        bool depthOnlyMode { false };
        bool depthBiasEnable { false };

        // Specialization constants
        std::optional<vk::SpecializationInfo> fragmentSpecialization;

        PipelineBuilder(VulkanContext* context);
        PipelineBuilder(VulkanContext* context, const str& vertFilePath, const str& fragFilePath);

        void clear();

        [[nodiscard]] uptr<MaterialPipeline> buildPipeline(vk::Device device) const;

        void setShaders(vk::ShaderModule vertexShader, vk::ShaderModule fragmentShader);

        void setInputTopology(vk::PrimitiveTopology topology);

        void setPolygonMode(vk::PolygonMode mode);

        void setCullMode(vk::CullModeFlags cullMode, vk::FrontFace frontFace);

        void setMultisamplingNone();

        void disableBlending();

        void enableBlendingAdditive();

        void enableBlendingAlphaBlend();

        void setColorAttachmentFormat(vk::Format format);

        void setColorAttachmentFormats(std::span<vk::Format> formats);

        void setDepthFormat(vk::Format format);

        void disableDepthTest();

        void enableDepthTest(bool deepWriteEnable, vk::CompareOp compareOp);

        void setDepthBiasEnable(bool enable);

        void setDepthOnlyMode(bool enable);

        void setVertexShaderOnly(vk::ShaderModule vertexShader);

        void setFragmentSpecialization(const vk::SpecializationInfo& specInfo);

        void destroyShaderModules(vk::Device device);
    };
} // namespace graphics
