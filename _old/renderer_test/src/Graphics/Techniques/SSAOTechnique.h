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

    struct SSAOParams {
        float radius = 5.0f;        // SSAO sampling radius (in world units)
        float bias = 0.025f;        // Depth bias to prevent self-occlusion
        float intensity = 1.5f;     // SSAO intensity multiplier
        bool enabled = false;       // Disabled by default
        bool blurEnabled = true;    // Apply blur pass
        bool ssaoOnly = false;      // Debug: show SSAO only (multiply with white)
    };

    // Constants for SSAO
    constexpr int SSAO_KERNEL_SIZE = 64;
    constexpr int SSAO_NOISE_DIM = 4;

    class SSAOTechnique {
    public:
        void init(Renderer* renderer, uint32_t width, uint32_t height);
        void cleanup(vk::Device device);
        void resize(uint32_t width, uint32_t height);

        // Apply SSAO to input image using G-Buffer data
        // Reads from inputImage (scene color), positionImage, normalImage
        // Writes to outputImage (scene color * SSAO)
        void apply(vk::CommandBuffer cmd,
                   Image& inputImage,      // Scene color input
                   Image& outputImage,     // Scene color output (with SSAO applied)
                   Image& positionImage,   // G-Buffer position
                   Image& normalImage,     // G-Buffer normal
                   const Mat4& projection,
                   const Mat4& view,
                   DescriptorAllocatorGrowable& frameDescriptors);

        SSAOParams& getParams() { return params; }
        const SSAOParams& getParams() const { return params; }

    private:
        void createImages();
        void createNoiseTexture();
        void createKernel();
        void createPipelines();
        void createDescriptors();
        void destroyImages();

        Renderer* renderer { nullptr };
        VulkanContext* context { nullptr };
        SSAOParams params;

        uint32_t width { 0 };
        uint32_t height { 0 };

        // SSAO output images
        Image ssaoImage;        // Raw SSAO output (single channel)
        Image ssaoBlurImage;    // Blurred SSAO output (single channel)

        // SSAO noise texture
        Image noiseImage;

        // SSAO kernel buffer (hemisphere samples)
        Buffer kernelBuffer;

        // SSAO params buffer
        Buffer ssaoParamsBuffer;

        // Pipelines
        uptr<MaterialPipeline> ssaoPipeline;      // Generate SSAO
        uptr<MaterialPipeline> blurPipeline;      // Blur SSAO
        uptr<MaterialPipeline> compositePipeline; // Apply SSAO to scene

        // Pipeline layouts
        vk::PipelineLayout ssaoLayout { nullptr };
        vk::PipelineLayout blurLayout { nullptr };
        vk::PipelineLayout compositeLayout { nullptr };

        // Descriptor set layouts
        vk::DescriptorSetLayout ssaoDescriptorLayout { nullptr };
        vk::DescriptorSetLayout blurDescriptorLayout { nullptr };
        vk::DescriptorSetLayout compositeDescriptorLayout { nullptr };

        // Sampler
        vk::Sampler ssaoSampler { nullptr };
        vk::Sampler noiseSampler { nullptr };  // Repeat sampler for noise texture
    };

} // namespace graphics::techniques

