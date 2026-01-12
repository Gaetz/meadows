#pragma once

#include "IRenderingTechnique.h"
#include "../MaterialPipeline.h"
#include "../DescriptorAllocatorGrowable.h"
#include "../ShadowMap.h"

namespace graphics::techniques {

    class ShadowMappingTechnique : public IRenderingTechnique {
    public:
        void init(Renderer* renderer) override;
        void cleanup(vk::Device device) override;

        void render(
            vk::CommandBuffer cmd,
            const DrawContext& drawContext,
            const GPUSceneData& sceneData,
            DescriptorAllocatorGrowable& frameDescriptors
        ) override;

        bool requiresShadowPass() const override { return true; }
        const TechniqueType getTechnique() const override { return TechniqueType::ShadowMapping; }
        const str getName() const override { return std::move("ShadowMapping"); }

        ShadowMap* getShadowMap() { return shadowMap.get(); }
        vk::DescriptorSetLayout getMaterialLayout() const { return materialLayout; }

        void setDisplayShadowMap(bool display) { displayShadowMap = display; }
        bool isDisplayingShadowMap() const { return displayShadowMap; }

        void setEnablePCF(bool enable) { enablePCF = enable; }
        bool isPCFEnabled() const { return enablePCF; }

    private:
        void buildDepthPipeline(vk::Device device);
        void buildShadowMeshPipeline(vk::Device device);
        void buildDebugPipeline(vk::Device device);

        void renderShadowPass(vk::CommandBuffer cmd, const DrawContext& drawContext, const GPUSceneData& sceneData, DescriptorAllocatorGrowable& frameDescriptors);
        void renderShadowGeometry(vk::CommandBuffer cmd, const DrawContext& drawContext, const GPUSceneData& sceneData, vk::DescriptorSet sceneDescriptor);
        void renderDebugView(vk::CommandBuffer cmd, vk::DescriptorSet sceneDescriptor);

        Renderer* renderer { nullptr };

        uptr<ShadowMap> shadowMap;

        uptr<MaterialPipeline> depthPipeline;
        uptr<MaterialPipeline> shadowMeshPipeline;
        uptr<MaterialPipeline> debugPipeline;

        vk::PipelineLayout depthPipelineLayout { nullptr };
        vk::PipelineLayout shadowMeshPipelineLayout { nullptr };
        vk::PipelineLayout debugPipelineLayout { nullptr };
        vk::DescriptorSetLayout materialLayout { nullptr };
        vk::DescriptorSetLayout shadowSceneDataLayout { nullptr };

        bool displayShadowMap { false };
        bool enablePCF { true };

        MaterialInstance* lastMaterial { nullptr };
        vk::Buffer lastIndexBuffer { nullptr };
    };

} // namespace graphics::techniques
