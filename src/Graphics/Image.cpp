#include "Image.h"

#include "Buffer.h"
#include "Utils.hpp"
#include "VulkanContext.h"
#include "VulkanInit.hpp"

namespace graphics {
    Image::Image(VulkanContext *context, vk::Extent3D size, vk::Format format, vk::ImageUsageFlags usage,
                 bool mipmapped) : context(context), allocation(nullptr), imageExtent(size), imageFormat(format) {
        image = nullptr;
        imageView = nullptr;

        VkImageCreateInfo img_info = graphics::imageCreateInfo(format, usage, size);
        if (mipmapped) {
            img_info.mipLevels = static_cast<uint32_t>(std::floor(std::log2(std::max(size.width, size.height)))) + 1;
        }

        // Always allocate images on dedicated GPU memory
        VmaAllocationCreateInfo allocinfo {};
        allocinfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        allocinfo.requiredFlags = static_cast<VkMemoryPropertyFlags>(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        // Allocate and create the image
        VkImage newImage = VK_NULL_HANDLE;
        vmaCreateImage(context->getAllocator(), &img_info, &allocinfo, &newImage, &allocation, nullptr);
        image = newImage;

        // If the format is a depth format, we will need to have it use the correct aspect flag
        vk::ImageAspectFlags aspectFlag = vk::ImageAspectFlagBits::eColor;
        if (format == vk::Format::eD32Sfloat) {
            aspectFlag = vk::ImageAspectFlagBits::eDepth;
        }

        // Build an image-view for the image
        VkImageViewCreateInfo view_info = graphics::imageViewCreateInfo(format, image, aspectFlag);
        view_info.subresourceRange.levelCount = img_info.mipLevels;

        VkImageView newView = VK_NULL_HANDLE;
        vkCreateImageView(context->getDevice(), &view_info, nullptr, &newView);
        imageView = newView;
    }

    Image::Image(VulkanContext *context, ImmediateSubmitter& submitter, void *data, vk::Extent3D size, vk::Format format, vk::ImageUsageFlags usage, bool mipmapped)
        : Image(context, size, format, usage | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc, mipmapped) {

        size_t data_size = static_cast<size_t>(size.depth) * size.width * size.height * 4;
        Buffer uploadbuffer {context, data_size, vk::BufferUsageFlagBits::eTransferSrc, VMA_MEMORY_USAGE_CPU_TO_GPU};

        memcpy(uploadbuffer.info.pMappedData, data, data_size);

        submitter.immediateSubmit(context, [&](VkCommandBuffer cmd) {
            graphics::transitionImage(cmd, this->image, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal);

            VkBufferImageCopy copyRegion = {};
            copyRegion.bufferOffset = 0;
            copyRegion.bufferRowLength = 0;
            copyRegion.bufferImageHeight = 0;

            copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copyRegion.imageSubresource.mipLevel = 0;
            copyRegion.imageSubresource.baseArrayLayer = 0;
            copyRegion.imageSubresource.layerCount = 1;
            copyRegion.imageExtent = size;

            // copy the buffer into the image
            vkCmdCopyBufferToImage(cmd, uploadbuffer.buffer, this->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                &copyRegion);

            graphics::transitionImage(cmd, this->image, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eReadOnlyOptimal);
            });
    }

    Image::Image(Image&& other) noexcept
        : context(other.context), image(other.image), allocation(other.allocation),
          imageView(other.imageView), imageExtent(other.imageExtent), imageFormat(other.imageFormat) {
        other.image = nullptr;
        other.imageView = nullptr;
        other.allocation = nullptr;
    }

    Image& Image::operator=(Image&& other) noexcept {
        if (this != &other) {
            context = other.context;
            image = other.image;
            allocation = other.allocation;
            imageView = other.imageView;
            imageExtent = other.imageExtent;
            imageFormat = other.imageFormat;

            other.image = nullptr;
            other.imageView = nullptr;
            other.allocation = nullptr;
        }
        return *this;
    }

    void Image::destroy(const VulkanContext* ctx) {
        if (ctx) {
            if (imageView) {
                ctx->getDevice().destroyImageView(imageView);
                imageView = nullptr;
            }
            if (image && allocation) {
                vmaDestroyImage(ctx->getAllocator(), image, allocation);
                image = nullptr;
                allocation = nullptr;
            }
        }
    }
}
