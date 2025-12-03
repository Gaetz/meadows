#include "VulkanInit.hpp"

#include "Buffer.h"

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

    vk::CommandBufferBeginInfo commandBufferBeginInfo(vk::CommandBufferUsageFlags flags)
    {
        vk::CommandBufferBeginInfo info{};
        info.pNext = nullptr;
        info.pInheritanceInfo = nullptr;
        info.flags = flags;
        return info;
    }

    vk::CommandBufferSubmitInfo commandBufferSubmitInfo(vk::CommandBuffer command)
    {
        vk::CommandBufferSubmitInfo info {};
        info.pNext = nullptr;
        info.commandBuffer = command;
        info.deviceMask = 0;
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

    vk::SemaphoreSubmitInfo semaphoreSubmitInfo(vk::Semaphore semaphore, vk::PipelineStageFlagBits2 stageMask) {
        vk::SemaphoreSubmitInfo info{};
        info.pNext = nullptr;
        info.semaphore = semaphore;
        info.stageMask = stageMask;
        info.deviceIndex = 0;
        info.value = 1;
        return info;
    }

    vk::ImageSubresourceRange imageSubresourceRange(vk::ImageAspectFlags aspectFlags) {
        vk::ImageSubresourceRange subImage{};
        subImage.aspectMask = aspectFlags;
        subImage.baseMipLevel = 0;
        subImage.levelCount = vk::RemainingMipLevels;
        subImage.baseArrayLayer = 0;
        subImage.layerCount = vk::RemainingArrayLayers;
        return subImage;
    }

    vk::SubmitInfo2 submitInfo(vk::CommandBufferSubmitInfo commandSubmitInfo,
                                 vk::SemaphoreSubmitInfo* signalSemaphoreInfo,
                                 vk::SemaphoreSubmitInfo* waitSemaphoreInfo) {
        vk::SubmitInfo2 info{};
        info.pNext = nullptr;

        info.waitSemaphoreInfoCount = waitSemaphoreInfo == nullptr ? 0 : 1;
        info.pWaitSemaphoreInfos = waitSemaphoreInfo;
        info.signalSemaphoreInfoCount = signalSemaphoreInfo == nullptr ? 0 : 1;
        info.pSignalSemaphoreInfos = signalSemaphoreInfo;
        info.commandBufferInfoCount = 1;
        info.pCommandBufferInfos = &commandSubmitInfo;

        return info;
    }

} // namespace graphics