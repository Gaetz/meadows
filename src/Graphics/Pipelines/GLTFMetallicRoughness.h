#pragma once

#include "Graphics/Types.h"
#include "Graphics/DescriptorWriter.h"
#include "Graphics/MaterialPipeline.h"

namespace graphics {
    class DescriptorAllocatorGrowable;
    class Renderer;
}

namespace  graphics::pipelines {

    class GLTFMetallicRoughness {
    public:
        uptr<MaterialPipeline> opaquePipeline { nullptr };
        uptr<MaterialPipeline> transparentPipeline { nullptr };
        vk::DescriptorSetLayout materialLayout { nullptr };
        vk::PipelineLayout pipelineLayout { nullptr };

        struct MaterialConstants {
            Vec4 colorFactors;
            Vec4 metalRoughFactors;
            // Padding, we need it anyway for uniform buffers
            glm::vec4 extra[14];
        };

        struct MaterialResources {
            Image colorImage;
            vk::Sampler colorSampler;
            Image metalRoughImage;
            vk::Sampler metalRoughSampler;
            vk::Buffer dataBuffer;
            u32 dataBufferOffset;
        };

        DescriptorWriter writer;

        void buildPipelines(const Renderer *renderer);
        void clear(vk::Device device);

        MaterialInstance writeMaterial(vk::Device device, MaterialPass pass, const MaterialResources& resources, DescriptorAllocatorGrowable* descriptorAllocator);
    };

}
