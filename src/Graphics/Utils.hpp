#pragma once

#include <functional>
#include <vulkan/vulkan.hpp>


namespace graphics
{
    class VulkanContext;

    vk::ShaderModule createShaderModule(const std::vector<char> &code, vk::Device device);
    void transitionImage(vk::CommandBuffer command, vk::Image image, vk::ImageLayout currentLayout, vk::ImageLayout newLayout);
    void transitionImage(vk::CommandBuffer command, vk::Image image, vk::ImageLayout currentLayout, vk::ImageLayout newLayout, vk::ImageAspectFlags aspectFlags);
    void copyImageToImage(vk::CommandBuffer command, vk::Image srcImage, vk::Image dstImage, vk::Extent2D srcSize, vk::Extent2D dstSize);
    void generateMipmaps(vk::CommandBuffer command, vk::Image image, vk::Extent2D imageSize);

    class ImmediateSubmitter {
    public:
        vk::Fence immFence;
        vk::CommandPool immCommandPool;
        vk::CommandBuffer immCommandBuffer;
        void immediateSubmit(VulkanContext* context, std::function<void(vk::CommandBuffer cmd)>&& function);
    };
}
