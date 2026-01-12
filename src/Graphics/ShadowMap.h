#pragma once

#include "Image.h"
#include "VulkanLoader.h"

namespace graphics {

    class VulkanContext;

    class ShadowMap {
    public:
        ShadowMap() = default;
        ShadowMap(VulkanContext* context, uint32_t resolution = 2048);
        ~ShadowMap() = default;

        void destroy();

        // Getters
        Image& getImage() { return depthImage; }
        const Image& getImage() const { return depthImage; }
        vk::Sampler getSampler() const { return sampler; }
        uint32_t getResolution() const { return resolution; }

        // Shadow configuration
        float getZNear() const { return zNear; }
        float getZFar() const { return zFar; }
        float getDepthBiasConstant() const { return depthBiasConstant; }
        float getDepthBiasSlope() const { return depthBiasSlope; }

        void setZNear(float value) { zNear = value; }
        void setZFar(float value) { zFar = value; }
        void setDepthBiasConstant(float value) { depthBiasConstant = value; }
        void setDepthBiasSlope(float value) { depthBiasSlope = value; }

        // Move semantics
        ShadowMap(ShadowMap&& other) noexcept;
        ShadowMap& operator=(ShadowMap&& other) noexcept;

        // No copy
        ShadowMap(const ShadowMap&) = delete;
        ShadowMap& operator=(const ShadowMap&) = delete;

    private:
        void createSampler();

        VulkanContext* context { nullptr };
        Image depthImage;
        vk::Sampler sampler { nullptr };
        uint32_t resolution { 2048 };

        // Shadow configuration
        float zNear { 1.0f };
        float zFar { 96.0f };
        float depthBiasConstant { 1.25f };
        float depthBiasSlope { 1.75f };
    };

} // namespace graphics
