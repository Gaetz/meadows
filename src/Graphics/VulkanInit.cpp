#include "VulkanInit.hpp"

#include <vulkan/vulkan_structs.hpp>
#include <vulkan/vulkan_structs.hpp>

namespace graphics
{
    vk::CommandPoolCreateInfo commandPoolCreateInfo(u32 queueFamilyIndex, vk::CommandPoolCreateFlags flags)
    {
        vk::CommandPoolCreateInfo info{};
        info.queueFamilyIndex = queueFamilyIndex;
        info.flags = flags;
        return info;
    }

    vk::CommandBufferAllocateInfo commandBufferAllocateInfo(vk::CommandPool pool, u32 count)
    {
        vk::CommandBufferAllocateInfo info{};
        info.commandPool = pool;
        info.level = vk::CommandBufferLevel::ePrimary;
        info.commandBufferCount = count;
        return info;
    }

    vk::FenceCreateInfo fenceCreateInfo(vk::FenceCreateFlags flags)
    {
        vk::FenceCreateInfo info{};
        info.flags = flags;
        return info;
    }

    vk::SemaphoreCreateInfo semaphoreCreateInfo(vk::SemaphoreCreateFlags flags)
    {
        vk::SemaphoreCreateInfo info{};
        info.flags = flags;
        return info;
    }
} // namespace graphics