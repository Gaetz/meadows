#pragma once
#include <vulkan/vulkan_structs.hpp>
#include "../Defines.h"

namespace graphics
{
    vk::CommandPoolCreateInfo commandPoolCreateInfo(u32 queueFamilyIndex, vk::CommandPoolCreateFlags flags = {});
    vk::CommandBufferAllocateInfo commandBufferAllocateInfo(vk::CommandPool pool, u32 count);
    vk::FenceCreateInfo fenceCreateInfo(vk::FenceCreateFlags flags = {});
    vk::SemaphoreCreateInfo semaphoreCreateInfo(vk::SemaphoreCreateFlags flags = {});
}
