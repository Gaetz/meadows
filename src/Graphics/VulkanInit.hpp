#pragma once
#include <vulkan/vulkan.hpp>
#include "../Defines.h"

namespace graphics
{
    vk::CommandPoolCreateInfo commandPoolCreateInfo(u32 queueFamilyIndex, vk::CommandPoolCreateFlags flags = {});
    vk::CommandBufferAllocateInfo commandBufferAllocateInfo(vk::CommandPool pool, u32 count);
    vk::CommandBufferBeginInfo commandBufferBeginInfo(vk::CommandBufferUsageFlags flags);
    vk::CommandBufferSubmitInfo commandBufferSubmitInfo(vk::CommandBuffer command);

    vk::FenceCreateInfo fenceCreateInfo(vk::FenceCreateFlags flags = {});
    vk::SemaphoreCreateInfo semaphoreCreateInfo(vk::SemaphoreCreateFlags flags = {});
    vk::SemaphoreSubmitInfo semaphoreSubmitInfo(vk::Semaphore semaphore, vk::PipelineStageFlagBits2 flags = {});

    vk::ImageSubresourceRange imageSubresourceRange(vk::ImageAspectFlags aspectFlags);
    vk::SubmitInfo2 submitInfo(vk::CommandBufferSubmitInfo commandSubmitInfo,
        vk::SemaphoreSubmitInfo* signalSemaphoreInfo, vk::SemaphoreSubmitInfo* waitSemaphoreInfo);

    vk::ImageCreateInfo imageCreateInfo(vk::Format format, vk::ImageUsageFlags usageFlags, vk::Extent3D extent);
    vk::ImageViewCreateInfo imageViewCreateInfo(vk::Format format, vk::Image image, vk::ImageAspectFlags aspectFlags);

    vk::RenderingAttachmentInfo attachmentInfo(vk::ImageView imageView, vk::ClearValue* clear, vk::ImageLayout imageLayout);
    vk::RenderingInfo renderingInfo(vk::Rect2D renderArea, vk::RenderingAttachmentInfo* colorAttachments,
        vk::RenderingAttachmentInfo* depthAttachment = nullptr, vk::RenderingAttachmentInfo* stencilAttachment = nullptr);

}
