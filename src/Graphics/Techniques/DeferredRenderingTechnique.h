#pragma once

#include "IRenderingTechnique.h"
#include "../Image.h"
#include "../MaterialPipeline.h"
#include "../DescriptorAllocatorGrowable.h"

namespace graphics::techniques {

    struct GBuffer {
        Image position;
        Image normal;
        Image albedo;
        vk::Extent3D extent;

        void init(VulkanContext* context, vk::Extent3D extent);
        void destroy(VulkanContext* context);
    };

    class DeferredRenderingTechnique : public IRenderingTechnique {
    public:
        enum class DebugMode : uint32_t {
            None = 0,
            Position = 1,
            Normal = 2,
            Albedo = 3
        };

        void init(Renderer* renderer) override;
        void cleanup(vk::Device device) override;
        void render(vk::CommandBuffer cmd, const DrawContext& drawContext, const GPUSceneData& sceneData, DescriptorAllocatorGrowable& frameDescriptors) override;

        bool requiresShadowPass() const override { return false; }
        const TechniqueType getTechnique() const override { return TechniqueType::Deferred; }
        const str getName() const override { return "Deferred Rendering"; }

        void setDebugMode(DebugMode mode) { debugMode = mode; }
        DebugMode getDebugMode() const { return debugMode; }

    private:
        void createGBuffer();
        void createPipelines();
        void createDescriptors();

        Renderer* renderer { nullptr };
        GBuffer gBuffer;
        DebugMode debugMode = DebugMode::None;

        uptr<MaterialPipeline> gBufferPipeline;
        uptr<MaterialPipeline> deferredPipeline;

        vk::PipelineLayout gBufferLayout { nullptr };
        vk::PipelineLayout deferredLayout { nullptr };

        vk::DescriptorSetLayout gBufferDescriptorLayout { nullptr };
        vk::DescriptorSetLayout deferredDescriptorLayout { nullptr };
        
        vk::DescriptorSet gBufferDescriptorSet; // For the deferred pass to read G-Buffer
    };

} // namespace graphics::techniques
