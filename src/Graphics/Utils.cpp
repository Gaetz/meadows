#include "Utils.hpp"

#include "VulkanInit.hpp"

namespace graphics
{
    void transitionImage(vk::CommandBuffer command, vk::Image image,
        vk::ImageLayout currentLayout, vk::ImageLayout newLayout)
    {
        vk::ImageMemoryBarrier2 imageBarrier {};
        imageBarrier.pNext = nullptr;

        /*
         * This is inefficient, as it will stall the GPU pipeline a bit.
         * For our needs, its going to be fine as we are only going to do a
         * few transitions per frame. If you are doing many transitions per
         * frame as part of a post-process chain, you want to avoid doing this,
         * and instead use StageMasks more accurate to what you are doing.
         * cf.: https://github.com/KhronosGroup/Vulkan-Docs/wiki/Synchronization-Examples
         */
        imageBarrier.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands;
        imageBarrier.srcAccessMask = vk::AccessFlagBits2::eMemoryWrite;
        imageBarrier.dstStageMask = vk::PipelineStageFlagBits2::eAllCommands;
        imageBarrier.dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;

        imageBarrier.oldLayout = currentLayout;
        imageBarrier.newLayout = newLayout;

        vk::ImageAspectFlags imageAspectFlags = newLayout == vk::ImageLayout::eDepthStencilAttachmentOptimal ?
            vk::ImageAspectFlagBits::eDepth :
            vk::ImageAspectFlagBits::eColor;
        imageBarrier.subresourceRange = graphics::imageSubresourceRange(imageAspectFlags);
        imageBarrier.image = image;

        vk::DependencyInfo dependencyInfo {};
        dependencyInfo.imageMemoryBarrierCount = 1;
        dependencyInfo.pImageMemoryBarriers = &imageBarrier;

        command.pipelineBarrier2(dependencyInfo);
    }

    void copyImageToImage(vk::CommandBuffer command, vk::Image srcImage, vk::Image dstImage, vk::Extent2D srcSize,
        vk::Extent2D dstSize) {
        vk::ImageBlit2 blitRegion{};

        blitRegion.srcOffsets[1].x = srcSize.width;
        blitRegion.srcOffsets[1].y = srcSize.height;
        blitRegion.srcOffsets[1].z = 1;

        blitRegion.dstOffsets[1].x = dstSize.width;
        blitRegion.dstOffsets[1].y = dstSize.height;
        blitRegion.dstOffsets[1].z = 1;

        blitRegion.srcSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
        blitRegion.srcSubresource.baseArrayLayer = 0;
        blitRegion.srcSubresource.layerCount = 1;
        blitRegion.srcSubresource.mipLevel = 0;

        blitRegion.dstSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
        blitRegion.dstSubresource.baseArrayLayer = 0;
        blitRegion.dstSubresource.layerCount = 1;
        blitRegion.dstSubresource.mipLevel = 0;

        vk::BlitImageInfo2 blitInfo{};
        blitInfo.dstImage = dstImage;
        blitInfo.dstImageLayout = vk::ImageLayout::eTransferDstOptimal;
        blitInfo.srcImage = srcImage;
        blitInfo.srcImageLayout = vk::ImageLayout::eTransferSrcOptimal;
        blitInfo.filter = vk::Filter::eLinear;
        blitInfo.regionCount = 1;
        blitInfo.pRegions = &blitRegion;

        command.blitImage2(&blitInfo);
    }
}
