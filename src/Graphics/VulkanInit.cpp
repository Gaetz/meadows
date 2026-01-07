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

    vk::SubmitInfo2 submitInfo(const vk::CommandBufferSubmitInfo* commandSubmitInfo,
                                 vk::SemaphoreSubmitInfo* signalSemaphoreInfo,
                                 vk::SemaphoreSubmitInfo* waitSemaphoreInfo) {
        vk::SubmitInfo2 info{};
        info.pNext = nullptr;

        info.waitSemaphoreInfoCount = waitSemaphoreInfo == nullptr ? 0 : 1;
        info.pWaitSemaphoreInfos = waitSemaphoreInfo;
        info.signalSemaphoreInfoCount = signalSemaphoreInfo == nullptr ? 0 : 1;
        info.pSignalSemaphoreInfos = signalSemaphoreInfo;
        info.commandBufferInfoCount = commandSubmitInfo == nullptr ? 0 : 1;
        info.pCommandBufferInfos = commandSubmitInfo;

        return info;
    }

    vk::ImageCreateInfo imageCreateInfo(vk::Format format, vk::ImageUsageFlags usageFlags, vk::Extent3D extent) {
        vk::ImageCreateInfo info{};
        info.imageType = vk::ImageType::e2D;
        info.format = format;
        info.extent = extent;
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = vk::SampleCountFlagBits::e1;
        info.tiling = vk::ImageTiling::eOptimal;
        info.usage = usageFlags;
        return info;
    }

    vk::ImageViewCreateInfo imageViewCreateInfo(vk::Format format, vk::Image image, vk::ImageAspectFlags aspectFlags) {
        vk::ImageViewCreateInfo info{};
        info.image = image;
        info.viewType = vk::ImageViewType::e2D;
        info.format = format;
        info.subresourceRange.baseMipLevel = 0;
        info.subresourceRange.levelCount = 1;
        info.subresourceRange.baseArrayLayer = 0;
        info.subresourceRange.layerCount = 1;
        info.subresourceRange.aspectMask = aspectFlags;
        return info;
    }

    vk::RenderingAttachmentInfo attachmentInfo(vk::ImageView imageView, vk::ClearValue* clear, vk::ImageLayout imageLayout) {
        vk::RenderingAttachmentInfo info{};
        info.imageView = imageView;
        info.imageLayout = imageLayout;
        info.loadOp = clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad;
        if (clear) {
            info.clearValue = *clear;
        }
        info.storeOp = vk::AttachmentStoreOp::eStore;
        return info;
    }

    vk::RenderingAttachmentInfo depthAttachmentInfo(const vk::ImageView imageView, const vk::ImageLayout imageLayout) {
        vk::RenderingAttachmentInfo info{};
        info.imageView = imageView;
        info.imageLayout = imageLayout;
        info.loadOp = vk::AttachmentLoadOp::eClear;
        info.storeOp = vk::AttachmentStoreOp::eStore;
        info.clearValue.depthStencil.depth = 0.f;
        return info;
    }

    vk::RenderingInfo renderingInfo(vk::Rect2D renderArea, vk::RenderingAttachmentInfo* colorAttachments,
        vk::RenderingAttachmentInfo* depthAttachment, vk::RenderingAttachmentInfo* stencilAttachment) {
        vk::RenderingInfo info{};
        info.renderArea = renderArea;
        info.layerCount = 1;
        info.colorAttachmentCount = 1;
        info.pColorAttachments = colorAttachments;
        info.pDepthAttachment = depthAttachment;
        info.pStencilAttachment = stencilAttachment;
        return info;
    }

    vk::PipelineShaderStageCreateInfo shaderStageCreateInfo(vk::ShaderStageFlagBits stage,
        vk::ShaderModule shaderModule) {
        vk::PipelineShaderStageCreateInfo info{};
        info.stage = stage;
        info.module = shaderModule;
        info.pName = "main";
        return info;
    }
} // namespace graphics