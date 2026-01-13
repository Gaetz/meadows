#pragma once

#include "../Image.h"
#include "../Buffer.h"
#include "../MaterialPipeline.h"
#include "../DescriptorAllocatorGrowable.h"

namespace graphics {
    class Renderer;
    class VulkanContext;
}

namespace graphics::techniques {

    struct BloomParams {
        float threshold = 0.8f;
        float intensity = 1.0f;
        float blurScale = 1.0f;
        float blurStrength = 1.5f;
        float bloomStrength = 0.5f;
        float exposure = 1.0f;
        bool enabled = false;  // Disabled by default
    };

    class BloomTechnique {
    public:
        void init(Renderer* renderer, uint32_t width, uint32_t height);
        void cleanup(vk::Device device);
        void resize(uint32_t width, uint32_t height);

        // Apply bloom to the input image and output to the final image
        void apply(vk::CommandBuffer cmd, Image& inputImage, Image& outputImage,
                   DescriptorAllocatorGrowable& frameDescriptors);

        BloomParams& getParams() { return params; }
        const BloomParams& getParams() const { return params; }

    private:
        void createImages();
        void createPipelines();
        void createDescriptors();
        void destroyImages();

        Renderer* renderer { nullptr };
        VulkanContext* context { nullptr };
        BloomParams params;

        uint32_t width { 0 };
        uint32_t height { 0 };

        // Bloom framebuffer images (half resolution for performance)
        Image brightPassImage;
        Image blurImageH;  // Horizontal blur result
        Image blurImageV;  // Vertical blur result

        // Pipelines
        uptr<MaterialPipeline> brightPassPipeline;
        uptr<MaterialPipeline> blurVertPipeline;
        uptr<MaterialPipeline> blurHorzPipeline;
        uptr<MaterialPipeline> compositePipeline;

        // Pipeline layouts
        vk::PipelineLayout brightPassLayout { nullptr };
        vk::PipelineLayout blurLayout { nullptr };
        vk::PipelineLayout compositeLayout { nullptr };

        // Descriptor set layouts
        vk::DescriptorSetLayout singleImageLayout { nullptr };
        vk::DescriptorSetLayout blurDescriptorLayout { nullptr };
        vk::DescriptorSetLayout compositeDescriptorLayout { nullptr };

        // Uniform buffer for blur params
        Buffer blurParamsBuffer;

        // Sampler
        vk::Sampler bloomSampler { nullptr };
    };

} // namespace graphics::techniques
