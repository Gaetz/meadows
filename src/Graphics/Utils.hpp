#pragma once

#include <vulkan/vulkan.hpp>

namespace graphics
{
    vk::ShaderModule createShaderModule(const std::vector<char> &code, vk::Device device);
    void transitionImage(vk::CommandBuffer command, vk::Image image, vk::ImageLayout currentLayout, vk::ImageLayout newLayout);
    void copyImageToImage(vk::CommandBuffer command, vk::Image srcImage, vk::Image dstImage, vk::Extent2D srcSize, vk::Extent2D dstSize);
}
