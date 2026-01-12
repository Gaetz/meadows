#pragma once

#include "Graphics/Types.h"
#include "Graphics/MaterialPipeline.h"

namespace graphics {
    class Renderer;
    class DescriptorAllocatorGrowable;
}

namespace graphics::pipelines {

    class ShadowPipeline {
    public:
        // Pipeline for shadow depth pass (renders from light's perspective)
        uptr<MaterialPipeline> depthPipeline { nullptr };

        // Pipeline for rendering scene with shadows
        uptr<MaterialPipeline> shadowMeshPipeline { nullptr };

        // Pipeline for debug shadow map visualization
        uptr<MaterialPipeline> debugPipeline { nullptr };

        // Pipeline layouts
        vk::PipelineLayout depthPipelineLayout { nullptr };
        vk::PipelineLayout shadowMeshPipelineLayout { nullptr };
        vk::PipelineLayout debugPipelineLayout { nullptr };

        // Material layout for shadow mesh (same as GLTFMetallicRoughness)
        vk::DescriptorSetLayout materialLayout { nullptr };

        void buildPipelines(const Renderer* renderer);
        void clear(vk::Device device);

    private:
        void buildDepthPipeline(const Renderer* renderer, vk::Device device);
        void buildShadowMeshPipeline(const Renderer* renderer, vk::Device device);
        void buildDebugPipeline(const Renderer* renderer, vk::Device device);
    };

} // namespace graphics::pipelines
