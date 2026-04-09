#pragma once

#include <functional>
#include <vulkan/vulkan.hpp>
#include "RenderObject.h"


namespace graphics
{
    class VulkanContext;

    vk::ShaderModule createShaderModule(const std::vector<char> &code, vk::Device device);
    void transitionImage(vk::CommandBuffer command, vk::Image image, vk::ImageLayout currentLayout, vk::ImageLayout newLayout);
    void transitionImage(vk::CommandBuffer command, vk::Image image, vk::ImageLayout currentLayout, vk::ImageLayout newLayout, vk::ImageAspectFlags aspectFlags);
    void copyImageToImage(vk::CommandBuffer command, vk::Image srcImage, vk::Image dstImage, vk::Extent2D srcSize, vk::Extent2D dstSize);
    void generateMipmaps(vk::CommandBuffer command, vk::Image image, vk::Extent2D imageSize);

    // Returns true if the object's bounding box intersects the view frustum defined by viewProj.
    bool isVisible(const RenderObject& obj, const Mat4& viewProj);

    // Binds a full-image viewport and scissor on the command buffer.
    inline void setViewportScissor(vk::CommandBuffer cmd, vk::Extent2D extent) {
        vk::Viewport viewport{};
        viewport.x        = 0;
        viewport.y        = 0;
        viewport.width    = static_cast<float>(extent.width);
        viewport.height   = static_cast<float>(extent.height);
        viewport.minDepth = 0.f;
        viewport.maxDepth = 1.f;
        cmd.setViewport(0, 1, &viewport);

        vk::Rect2D scissor{{0, 0}, {extent.width, extent.height}};
        cmd.setScissor(0, 1, &scissor);
    }

    class ImmediateSubmitter {
    public:
        vk::Fence immFence;
        vk::CommandPool immCommandPool;
        vk::CommandBuffer immCommandBuffer;
        void immediateSubmit(VulkanContext* context, std::function<void(vk::CommandBuffer cmd)>&& function);
    };
}
