#pragma once

#include <vulkan/vulkan.hpp>

namespace graphics
{
    void transitionImage(vk::CommandBuffer command, vk::Image image, vk::ImageLayout currentLayout, vk::ImageLayout newLayout);
    void copyImageToImage(vk::CommandBuffer command, vk::Image srcImage, vk::Image dstImage, vk::Extent2D srcSize, vk::Extent2D dstSize);
}
