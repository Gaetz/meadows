#include "ShadowMap.h"
#include "VulkanContext.h"

namespace graphics {

    ShadowMap::ShadowMap(VulkanContext* context, uint32_t resolution)
        : context(context), resolution(resolution) {

        // Create depth image for shadow map
        vk::Extent3D extent = { resolution, resolution, 1 };
        vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eDepthStencilAttachment
                                  | vk::ImageUsageFlagBits::eSampled;

        depthImage = Image(context, extent, vk::Format::eD16Unorm, usage, false);

        // Create sampler for shadow map
        createSampler();
    }

    void ShadowMap::createSampler() {
        vk::SamplerCreateInfo samplerInfo{};
        samplerInfo.magFilter = vk::Filter::eLinear;
        samplerInfo.minFilter = vk::Filter::eLinear;
        samplerInfo.mipmapMode = vk::SamplerMipmapMode::eLinear;
        samplerInfo.addressModeU = vk::SamplerAddressMode::eClampToBorder;
        samplerInfo.addressModeV = vk::SamplerAddressMode::eClampToBorder;
        samplerInfo.addressModeW = vk::SamplerAddressMode::eClampToBorder;
        samplerInfo.mipLodBias = 0.0f;
        samplerInfo.maxAnisotropy = 1.0f;
        samplerInfo.minLod = 0.0f;
        samplerInfo.maxLod = 1.0f;
        // Use white border color so areas outside shadow map are lit
        samplerInfo.borderColor = vk::BorderColor::eFloatOpaqueWhite;
        // Enable depth comparison for shadow mapping
        samplerInfo.compareEnable = VK_FALSE;

        sampler = context->getDevice().createSampler(samplerInfo);
    }

    void ShadowMap::destroy() {
        if (context) {
            if (sampler) {
                context->getDevice().destroySampler(sampler);
                sampler = nullptr;
            }
            depthImage.destroy(context);
        }
    }

    ShadowMap::ShadowMap(ShadowMap&& other) noexcept
        : context(other.context)
        , depthImage(std::move(other.depthImage))
        , sampler(other.sampler)
        , resolution(other.resolution)
        , zNear(other.zNear)
        , zFar(other.zFar)
        , depthBiasConstant(other.depthBiasConstant)
        , depthBiasSlope(other.depthBiasSlope) {
        other.sampler = nullptr;
        other.context = nullptr;
    }

    ShadowMap& ShadowMap::operator=(ShadowMap&& other) noexcept {
        if (this != &other) {
            destroy();

            context = other.context;
            depthImage = std::move(other.depthImage);
            sampler = other.sampler;
            resolution = other.resolution;
            zNear = other.zNear;
            zFar = other.zFar;
            depthBiasConstant = other.depthBiasConstant;
            depthBiasSlope = other.depthBiasSlope;

            other.sampler = nullptr;
            other.context = nullptr;
        }
        return *this;
    }

} // namespace graphics
