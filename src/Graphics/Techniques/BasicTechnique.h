#pragma once

#include "IRenderingTechnique.h"
#include "../MaterialPipeline.h"
#include "../DescriptorAllocatorGrowable.h"

namespace graphics::techniques {

    class BasicTechnique : public IRenderingTechnique {
    public:
        void init(Renderer* renderer) override;
        void cleanup(vk::Device device) override;

        void render(
            vk::CommandBuffer cmd,
            const DrawContext& drawContext,
            const GPUSceneData& sceneData,
            DescriptorAllocatorGrowable& frameDescriptors
        ) override;

        bool requiresShadowPass() const override { return false; }
        const char* getName() const override { return "Basic"; }

        vk::DescriptorSetLayout getMaterialLayout() const { return materialLayout; }
        vk::PipelineLayout getPipelineLayout() const { return pipelineLayout; }
        MaterialPipeline* getOpaquePipeline() const { return opaquePipeline.get(); }
        MaterialPipeline* getTransparentPipeline() const { return transparentPipeline.get(); }

    private:
        void renderGeometry(
            vk::CommandBuffer cmd,
            const DrawContext& drawContext,
            const GPUSceneData& sceneData,
            vk::DescriptorSet globalDescriptor
        );

        Renderer* renderer { nullptr };

        uptr<MaterialPipeline> opaquePipeline;
        uptr<MaterialPipeline> transparentPipeline;
        vk::DescriptorSetLayout materialLayout { nullptr };
        vk::PipelineLayout pipelineLayout { nullptr };

        MaterialPipeline* lastPipeline { nullptr };
        MaterialInstance* lastMaterial { nullptr };
        vk::Buffer lastIndexBuffer { nullptr };
    };

} // namespace graphics::techniques
