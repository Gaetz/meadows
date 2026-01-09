#pragma once

#include <functional>
#include <vulkan/vulkan.hpp>


namespace graphics
{
    class VulkanContext;

    vk::ShaderModule createShaderModule(const std::vector<char> &code, vk::Device device);
    void transitionImage(vk::CommandBuffer command, vk::Image image, vk::ImageLayout currentLayout, vk::ImageLayout newLayout);
    void copyImageToImage(vk::CommandBuffer command, vk::Image srcImage, vk::Image dstImage, vk::Extent2D srcSize, vk::Extent2D dstSize);

    class ImmediateSubmitter {
    public:
        vk::Fence immFence;
        vk::CommandPool immCommandPool;
        vk::CommandBuffer immCommandBuffer;
        void immediateSubmit(VulkanContext* context, std::function<void(vk::CommandBuffer cmd)>&& function);
    };
}
