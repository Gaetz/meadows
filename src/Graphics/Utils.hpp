#pragma once

#include <vulkan/vulkan.hpp>

namespace graphics
{
    void transitionImage(vk::CommandBuffer command, vk::Image image, vk::ImageLayout currentLayout, vk::ImageLayout newLayout);
}
