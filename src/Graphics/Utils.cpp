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
}
